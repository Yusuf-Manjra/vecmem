/*
 * VecMem project, part of the ACTS project (R&D line)
 *
 * (c) 2021-2022 CERN for the benefit of the ACTS project
 *
 * Mozilla Public License Version 2.0
 */

// Local include(s).
#include "binary_page_memory_resource_impl.hpp"

#include "vecmem/utils/debug.hpp"

// System include(s).
#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <stdexcept>

#ifdef VECMEM_HAVE_LZCNT_U64
#include <intrin.h>
#endif

namespace {
/**
 * @brief Rounds a size up to the nearest power of two, and returns the power
 * (not the size itself).
 */
std::size_t round_up(std::size_t size) {
    for (std::size_t i = 0; i < 32; i++) {
        if ((static_cast<std::size_t>(1UL) << i) >= size) {
            return i;
        }
    }

    return 0;
}

inline std::size_t clzl(std::size_t i) {
#if defined(VECMEM_HAVE_LZCNT_U64)
    return _lzcnt_u64(i);
#elif defined(VECMEM_HAVE_BUILTIN_CLZL)
    return __builtin_clzl(i);
#else
    std::size_t b;
    for (b = 0;
         !((i << b) & (static_cast<std::size_t>(1UL)
                       << (std::numeric_limits<std::size_t>::digits - 1UL)));
         ++b)
        ;
    return b;
#endif
}
}  // namespace

namespace vecmem::details {

binary_page_memory_resource_impl::binary_page_memory_resource_impl(
    memory_resource &upstream)
    : m_upstream(upstream) {}

void *binary_page_memory_resource_impl::do_allocate(std::size_t size,
                                                    std::size_t) {
    /*
     * First, we round our allocation request up to a power of two, since
     * that is what the sizes of all our pages are.
     */
    std::size_t goal = std::max(min_page_size, round_up(size));

    VECMEM_DEBUG_MSG(3,
                     "Request received to allocate %ld bytes, looking for page "
                     "of size 2^%ld bytes",
                     size, goal);

    /*
     * Attempt to find a free page that can fit our allocation goal.
     */
    std::optional<page_ref> cand = find_free_page(goal);

    /*
     * If we don't have a candidate, there is no available page that can fit
     * our request. First, we allocate a new root page from the upstream
     * allocator, and then look for that new page.
     */
    if (!cand) {
        VECMEM_DEBUG_MSG(
            5, "No suitable page found, requesting upstream allocation");
        allocate_upstream(goal);

        cand = find_free_page(goal);
    }

    /*
     * If there is still no candidate, something has gone wrong and we
     * cannot recover.
     */
    if (!cand) {
        VECMEM_DEBUG_MSG(5,
                         "No suitable page found after upstream allocation, "
                         "unrecoverable error");
        throw std::bad_alloc();
    }

    /*
     * If the page is split (but its children are all free), we will first
     * need to unsplit it.
     */
    if (cand->get_state() == page_state::SPLIT) {
        VECMEM_DEBUG_MSG(5, "Candidate page is split and must be unsplit");
        cand->unsplit();
    }

    /*
     * Keep splitting the page until we have reached our target size.
     */
    while (cand->get_size() > goal) {
        VECMEM_DEBUG_MSG(5, "Candidate page is of size 2^%lu and must be split",
                         cand->get_size());
        cand->split();
        cand = cand->left_child();
    }

    /*
     * Mark the page as occupied, then return the address.
     */

    cand->change_state_vacant_to_occupied();

    /*
     * Get the address of the resulting page.
     */
    void *res = cand->get_addr();

    VECMEM_DEBUG_MSG(2,
                     "Allocated %ld bytes in a page of size 2^%lu bytes with "
                     "index %lu and address %p",
                     size, goal, cand->get_index(), res);

    return res;
}

void binary_page_memory_resource_impl::do_deallocate(void *p, std::size_t s,
                                                     std::size_t) {
    VECMEM_DEBUG_MSG(2, "De-allocating memory at %p", p);

    /*
     * First, we will try to find the superpage in which our allocation exists,
     * which will significantly shrink our search space.
     */
    std::optional<std::reference_wrapper<superpage>> sp{};

    /*
     * We iterate over each superpage, checking whether it is possible for that
     * superpage to contain the allocation.
     */
    for (superpage &_sp : m_superpages) {
        /*
         * Check whether the pointer we have lies somewhere between the begin
         * and the end of the memory allocated by this superpage. If it does,
         * we have found our target superpage and we can return.
         */
        if (_sp.m_memory.get() <= p &&
            static_cast<void *>(_sp.m_memory.get() +
                                (static_cast<std::size_t>(1UL) << _sp.m_size)) >
                p) {
            sp = _sp;
            break;
        }
    }

    /*
     * Next, we find where in this superpage the allocation must exist; we
     * first calculate the log_2 of the allocation size (`goal`). Then we find
     * the number of the first page with that size (`p_min`). If we then take
     * the pointer offset between the deallocation pointer (`p`) and the start
     * of the superpage's memory space we arrive at an offset of `diff` bytes.
     * Dividing `diff` by the size of the page in which we will have allocated
     * the memory gives us the offset from the first page of that size, which
     * allows us to easily find the page we're looking for.
     */
    std::size_t goal = std::max(min_page_size, round_up(s));
    std::size_t p_min = 0;
    for (; page_ref(sp->get(), p_min).get_size() > goal; p_min = 2 * p_min + 1)
        ;
    std::ptrdiff_t diff =
        static_cast<std::byte *>(p) - sp->get().m_memory.get();

    /*
     * Finally, change the state of the page to vacant.
     */
    page_ref(*sp, p_min + (diff / (static_cast<std::size_t>(1UL) << goal)))
        .change_state_occupied_to_vacant();
}

std::optional<binary_page_memory_resource_impl::page_ref>
binary_page_memory_resource_impl::find_free_page(std::size_t size) {
    bool candidate_sp_found;

    /*
     * We will look for a free page by looking at all the pages of the exact
     * size we need, and we will only move to a bigger page size if none of the
     * superpages has a page of the right size.
     */
    do {
        /*
         * This will be our stopping condition; we'll keep track of whether any
         * of the superpages even has a page of the right size; if not, we're
         * stuck.
         */
        candidate_sp_found = false;

        for (superpage &sp : m_superpages) {
            /*
             * For each superpage, we check if the total size is enougn to
             * support the page size we're looking for. If not, we will never
             * find such a page in this superpage.
             */
            if (size <= sp.m_size) {
                candidate_sp_found = true;

                /*
                 * Calculate the index range of pages, from i to j, in which we
                 * can possibly find pages of the correct size.
                 */
                std::size_t i = 0;
                for (; page_ref(sp, i).get_size() > size; i = 2 * i + 1)
                    ;
                std::size_t j = 2 * i + 1;

                /*
                 * Iterate over the index range; if we find a free page, we
                 * know that we're done!
                 */
                for (std::size_t p = i; p < j; ++p) {
                    page_ref pr(sp, p);

                    /*
                     * Return the page, exiting the loop.
                     */
                    if (pr.get_state() == page_state::VACANT) {
                        return pr;
                    }
                }
            }
        }

        /*
         * If we find nothing, look for a page twice as big.
         */
        size++;
    } while (candidate_sp_found);

    /*
     * If we really can't find a fitting page, we return nothing.
     */
    return {};
}

void binary_page_memory_resource_impl::allocate_upstream(std::size_t size) {
    /*
     * Add our new page to the list of root pages.
     */
    m_superpages.emplace_back(std::max(size, new_page_size), m_upstream);
}

binary_page_memory_resource_impl::superpage::superpage(
    std::size_t size, memory_resource &resource)
    : m_size(size),
      m_num_pages((2UL << (m_size - min_page_size)) - 1),
      m_pages(std::make_unique<page_state[]>(m_num_pages)),
      m_memory(make_unique_alloc<std::byte[]>(
          resource, static_cast<std::size_t>(1UL) << m_size)) {
    /*
     * Set all pages as non-extant, except the first one.
     */
    for (std::size_t i = 0; i < m_num_pages; ++i) {
        if (i == 0) {
            m_pages[i] = page_state::VACANT;
        } else {
            m_pages[i] = page_state::NON_EXTANT;
        }
    }
}

std::size_t binary_page_memory_resource_impl::superpage::total_pages() const {
    return m_num_pages;
}

std::size_t binary_page_memory_resource_impl::page_ref::get_size() const {
    /*
     * Calculate the size of allocation represented by this page.
     */
    return (m_superpage.get().m_size -
            ((8 * sizeof(std::size_t) - 1) - clzl(m_page + 1)));
}

void binary_page_memory_resource_impl::page_ref::
    change_state_vacant_to_occupied() {
    assert(m_superpage.get().m_pages[m_page] == page_state::VACANT);
    m_superpage.get().m_pages[m_page] = page_state::OCCUPIED;
}

void binary_page_memory_resource_impl::page_ref::
    change_state_occupied_to_vacant() {
    assert(m_superpage.get().m_pages[m_page] == page_state::OCCUPIED);
    m_superpage.get().m_pages[m_page] = page_state::VACANT;
}

void binary_page_memory_resource_impl::page_ref::
    change_state_non_extant_to_vacant() {
    assert(m_superpage.get().m_pages[m_page] == page_state::NON_EXTANT);
    m_superpage.get().m_pages[m_page] = page_state::VACANT;
}

void binary_page_memory_resource_impl::page_ref::
    change_state_vacant_to_non_extant() {
    assert(m_superpage.get().m_pages[m_page] == page_state::VACANT);
    m_superpage.get().m_pages[m_page] = page_state::NON_EXTANT;
}

void binary_page_memory_resource_impl::page_ref::
    change_state_vacant_to_split() {
    assert(m_superpage.get().m_pages[m_page] == page_state::VACANT);
    m_superpage.get().m_pages[m_page] = page_state::SPLIT;
}

void binary_page_memory_resource_impl::page_ref::
    change_state_split_to_vacant() {
    assert(m_superpage.get().m_pages[m_page] == page_state::SPLIT);
    m_superpage.get().m_pages[m_page] = page_state::VACANT;
}

std::size_t binary_page_memory_resource_impl::page_ref::get_index() {
    return m_page;
}

binary_page_memory_resource_impl::page_ref::page_ref(superpage &s,
                                                     std::size_t p)
    : m_superpage(s), m_page(p) {
    assert(m_page < m_superpage.get().total_pages());
}

binary_page_memory_resource_impl::page_state
binary_page_memory_resource_impl::page_ref::get_state() const {
    return m_superpage.get().m_pages[m_page];
}

void *binary_page_memory_resource_impl::page_ref::get_addr() const {
    std::size_t d = m_superpage.get().m_size - get_size();

    return static_cast<void *>(
        &m_superpage.get()
             .m_memory[(m_page - ((static_cast<std::size_t>(1UL) << d) - 1)) *
                       (static_cast<std::size_t>(1UL) << get_size())]);
}

binary_page_memory_resource_impl::page_ref
binary_page_memory_resource_impl::page_ref::left_child() const {
    return {m_superpage, 2 * m_page + 1};
}
binary_page_memory_resource_impl::page_ref
binary_page_memory_resource_impl::page_ref::right_child() const {
    return {m_superpage, 2 * m_page + 2};
}

void binary_page_memory_resource_impl::page_ref::unsplit() {
    if (left_child().get_state() == page_state::SPLIT) {
        left_child().unsplit();
    }

    if (right_child().get_state() == page_state::SPLIT) {
        right_child().unsplit();
    }

    change_state_split_to_vacant();
    left_child().change_state_vacant_to_non_extant();
    right_child().change_state_vacant_to_non_extant();
}
void binary_page_memory_resource_impl::page_ref::split() {
    change_state_vacant_to_split();
    left_child().change_state_non_extant_to_vacant();
    right_child().change_state_non_extant_to_vacant();
}
}  // namespace vecmem::details
