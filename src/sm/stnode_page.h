/*
 * (c) Copyright 2011-2013, Hewlett-Packard Development Company, LP
 */

#ifndef STNODE_PAGE_H
#define STNODE_PAGE_H

#include "w_defines.h"
#include "generic_page.h"
#include "srwlock.h"
#include <vector>

class bf_fixed_m;



/**
 * \brief Persistent structure representing metadata for a store.
 *
 * \details
 * Contains the root page ID of the given store, store flags (e.g.,
 * what kind of logging to use), and the store's deleting status
 * (e.g., is the store in the process of being deleted?).
 *
 * These are contained in \ref stnode_page's.
 */
struct stnode_t {
    /// also okay to initialize via memset
    stnode_t() {
        root     = 0;
        flags    = 0;
        deleting = 0;
    }

    /// Root page ID of the store; holds 0 *if* the store is not in
    /// use (e.g., never created/deleted rather than being deleted).
    shpid_t         root;      // +4 -> 4
    /// store flags            (holds a smlevel_0::store_flag_t)
    uint16_t        flags;     // +2 -> 6
    /// store deleting status  (holds a smlevel_0::store_deleting_t)
    uint16_t        deleting;  // +2 -> 8
};


/**
 * \brief Store-node page that contains one stnode_t for each
 * (possibly deleted or uncreated) store belonging to a given volume.
 *
 * \details
 * The handle class for this class is stnode_page_h.
 */
class stnode_page : public generic_page_header {
    friend class stnode_page_h;

    /// max # \ref stnode_t's on a single stnode_page; thus, the
    /// maximum number of stores per volume
    static const size_t max = data_sz / sizeof(stnode_t);

    /// stnode[i] is the stnode_t for store # i of this volume
    stnode_t stnode[max];

    /// unused space (ideally of zero size)
    char*    padding[data_sz - sizeof(stnode)];
};



/**
 * \brief Handle for a stnode_page.
 */
class stnode_page_h {
    stnode_page *_page;

public:
    /// format given page with page-ID pid as an stnode_page page then
    /// return a handle to it
    stnode_page_h(generic_page* s, const lpid_t& pid);
    /// construct handle from an existing stnode_page page
    stnode_page_h(generic_page* s) : _page(reinterpret_cast<stnode_page*>(s)) {
        w_assert1(s->tag == t_stnode_p);
    }
    ~stnode_page_h() {}

    /// return pointer to underlying page
    generic_page* to_generic_page() const { return reinterpret_cast<generic_page*>(_page); }


    /// max # \ref stnode_t's on a single stnode_page; thus, the
    /// maximum number of stores per volume
    static const size_t max = stnode_page::max;

    stnode_t& get(size_t index) {
        // FIXME: it appears we do not ever use the stnode_t for the
        // store with # 0 as we use that number as a special case to
        // indicate stnode_page/alloc_page's.  See comment in
        // stnode_cache_t::get_min_unused_store_ID().  This is
        // demonstrated by the following assert never triggering:
        w_assert1(0 < index);

        w_assert1(index < max);
        return _page->stnode[index];
    }
    const stnode_t& get(size_t index) const {
        // see comment in non-const version of this method
        w_assert1(0 < index);

        w_assert1(index < max);
        return _page->stnode[index];
    }
};



/**
 * \brief Store creation/destroy/query interface.
 *
 * \details
 * This object handles store create/destroy/query requests for one
 * volume.  99.99% of the requests are, of course, querying the root
 * page ID of indexes.  This object does a lightweight synchronization
 * (latch) to protect them from MT accesses.  However, this object
 * doesn't use locks because we don't need them.  If the store is
 * being destroyed, ss_m will check intent locks before calling this
 * object, so we are safe.  
 *
 * This object and vol_t replace the "directory" thingies in original
 * Shore-MT with more efficiency and simplicity.
 */
class stnode_cache_t {
public:
    /// special_pages here holds the special pages for volume vid, the
    /// last of which should be the stnode_page for that volume
    stnode_cache_t(vid_t vid, bf_fixed_m* special_pages);
    
    /**
     * Returns the root page ID of the given store.
     * If that store isn't in use, returns 0.
     * @param[in] store Store ID.
     */
    shpid_t get_root_pid(snum_t store) const;
    
    /// Make a copy of the entire stnode_t of the given store.
    void get_stnode(snum_t store, stnode_t &stnode) const;

    /// Returns the first snum_t that can be used for a new store in
    /// this volume or stnode_page_h::max if all available stores of
    /// this volume are already in use.
    snum_t get_min_unused_store_ID() const;

    /// Returns the snum_t of all stores that exist (i.e., are not
    /// deleted and thus in use) in the volume.
    std::vector<snum_t> get_all_used_store_ID() const;


    /**
     *  Fix the stnode_page and perform the store operation *including* logging it.
     *
     *  param type is in sm_io.h.
     *
     *  It contains:
     *   typedef smlevel_0::store_operation_t        store_operation_t;
     *   in sm_base.h
     *   Operations:
     *       t_delete_store, <---- when really deleted after space freed
     *       t_create_store, <---- store is allocated (snum_t is in use)
     *       t_set_deleting, <---- when transaction deletes store (t_deleting_store)
     *       t_set_store_flags, 
     *
     *   typedef smlevel_0::store_flag_t             store_flag_t;
     *       in sm_base.h:
     *       logging attribute: regular, tmp, load, insert
     *
     *   typedef smlevel_0::store_deleting_t         store_deleting_t;
     *           t_not_deleting_store = 0,  // must be 0: code assumes it
     *           t_deleting_store, 
     *           t_unknown_deleting         // for error handling
     */
    rc_t  store_operation(const store_operation_param &op);


private:
    /// all operations in this object are protected by this lock
    mutable queue_based_lock_t _spin_lock;

    const vid_t   _vid;                /// The volume number of the volume we are caching 
    bf_fixed_m*   _special_pages;      /// The buffer manager holding the volume's special pages
    stnode_page_h _stnode_page;        /// The stnode_page of the volume we are caching
};

#endif // STNODE_PAGE_H