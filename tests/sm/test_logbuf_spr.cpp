/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

// modified from test_spr.cpp

#include "btree_test_env.h"
#include "generic_page.h"
#include "bf.h"
#include "btree.h"
#include "btree_page_h.h"
#include "btree_impl.h"
#include "log.h"
#include "w_error.h"
#include "backup.h"

#include "bf_fixed.h"
#include "bf_tree_cb.h"
#include "bf_tree.h"
#include "sm_io.h"
#include "sm_int_0.h"

#include <vector>

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>


btree_test_env *test_env;

const uint64_t SEG_SIZE = 1024*1024; // _segsize 1MB
const uint64_t LOG_SIZE = 128*1024*8;  // so that _partition_data_size is about 128MB 


const int APPEND_COUNT = 10000; // number of log comment log records (about 20KB) added


/**
 * \file test_spr.cpp
 * Tests for Single-page recovery.
 */

w_rc_t flush_and_evict(ss_m* ssm) {
    W_DO(ssm->force_buffers()); // clean them up
    // also, evict all to update EMLSN
    uint32_t evicted_count, unswizzled_count;
    W_DO(ssm->bf->evict_blocks(evicted_count, unswizzled_count, bf_tree_m::EVICT_COMPLETE));
    // then flush it, this time just for root node
    W_DO(ssm->force_buffers());
    return RCOK;
}

w_rc_t prepare_test(ss_m* ssm, test_volume_t *test_volume, stid_t &stid, lpid_t &root_pid,
                    shpid_t &target_pid, w_keystr_t &target_key0, w_keystr_t &target_key1) {
    W_DO(x_btree_create_index(ssm, test_volume, stid, root_pid));

    const int recsize = SM_PAGESIZE / 6;
    char datastr[recsize];
    ::memset (datastr, 'a', recsize);
    vec_t data;
    data.set(datastr, recsize);

    W_DO(ssm->begin_xct());
    w_keystr_t key;
    char keystr[6] = "";
    ::memset(keystr, '\0', 6);
    keystr[0] = 'k';
    keystr[1] = 'e';
    keystr[2] = 'y';
    for (int i = 0; i < 30; ++i) {
        keystr[3] = ('0' + ((i / 100) % 10));
        keystr[4] = ('0' + ((i / 10) % 10));
        keystr[5] = ('0' + ((i / 1) % 10));
        key.construct_regularkey(keystr, 6);
        test_env->set_xct_query_lock();
        W_DO(ssm->create_assoc(stid, key, data));
    }


    W_DO(ssm->commit_xct());

    W_DO(x_btree_verify(ssm, stid));
    W_DO(ssm->force_buffers()); // clean them up

    {
        btree_page_h root_p;
        W_DO(root_p.fix_root(root_pid.vol().vol, root_pid.store(), LATCH_SH));
        EXPECT_TRUE(root_p.nrecs() > 4);
        target_pid = root_p.child(1);
        btree_page_h target_p;
        W_DO(target_p.fix_nonroot(root_p, stid.vol.vol, target_pid, LATCH_SH));
        EXPECT_GE(2, target_p.nrecs());
        target_p.get_key(0, target_key0);
        target_p.get_key(1, target_key1);
    }
    W_DO(flush_and_evict(ssm));

    // then take a backup. this is the page image to start Single-Page-Recovery from.
    x_delete_backup(ssm, test_volume);
    W_DO(x_take_backup(ssm, test_volume));
    return RCOK;
}

void corrupt_page(test_volume_t *test_volume, shpid_t target_pid) {
    std::cout << "=========== Corrupting page " << target_pid << " in "
        << test_volume->_device_name << " for test===============" << std::endl;
    // Bypass bufferpool to corrupt it
    generic_page page;
    {
        std::ifstream file(test_volume->_device_name, std::ios::binary);
        file.seekg(sizeof(generic_page) * target_pid);
        file.read(reinterpret_cast<char*>(&page), sizeof(generic_page));
        file.close();
    }

    ::memset(reinterpret_cast<char*>(&page) + 1234, 42, 987);
//     {
//         std::ofstream file(test_volume->_device_name, std::ios::binary | std::ios::out);
//         file.seekp(sizeof(generic_page) * target_pid, ios_base::beg);
//         DBGOUT1(<<"setting seekp to " <<file.tellp());
//         file.write(reinterpret_cast<char*>(&page), sizeof(generic_page));
//         file.flush();
//         file.close();
//     } //This block corrupts 48K starting at offset 0 instead of a single page.
    {
        //This block works.
        int vol_fd = open(test_volume->_device_name, O_WRONLY);
        ssize_t written = pwrite(vol_fd, (char *)&page, sizeof(generic_page), sizeof(generic_page)*target_pid);
		EXPECT_EQ(written, (ssize_t)sizeof(generic_page));
        fsync(vol_fd);
        close(vol_fd);
    }
        
}
bool is_consecutive_chars(char* str, char c, int len) {
    for (int i = 0; i < len; ++i) {
        if (str[i] != c) {
            return false;
        }
    }
    return true;
}
w_rc_t test_nochange(ss_m* ssm, test_volume_t *test_volume) {
    stid_t stid;
    lpid_t root_pid;
    shpid_t target_pid;
    w_keystr_t target_key0, target_key1;
    W_DO (prepare_test(ssm, test_volume, stid, root_pid, target_pid, target_key0, target_key1));
    // no change after backup and immediately corrupt
    corrupt_page(test_volume, target_pid);

    // this should invoke Single-Page-Recovery with no REDO application
    char buf[SM_PAGESIZE / 6];
    smsize_t buf_len = SM_PAGESIZE / 6;
    bool found;
    W_DO(ssm->find_assoc(stid, target_key0, buf, buf_len, found));
    EXPECT_TRUE(found);
    EXPECT_EQ((smsize_t)(SM_PAGESIZE / 6), buf_len);
    EXPECT_TRUE(is_consecutive_chars(buf, 'a', SM_PAGESIZE / 6));
    W_DO(ssm->find_assoc(stid, target_key1, buf, buf_len, found));
    EXPECT_TRUE(found);
    EXPECT_EQ((smsize_t)(SM_PAGESIZE / 6), buf_len);
    EXPECT_TRUE(is_consecutive_chars(buf, 'a', SM_PAGESIZE / 6));

    //Clean up backup file
    BackupManager *bk = ssm->bk;
    volid_t vid = test_volume->_vid;
    x_delete_backup(ssm, test_volume);
    EXPECT_FALSE(bk->volume_exists(vid));
    
    return RCOK;
}
TEST (LogBufferTest_Spr, NoChange) {
    test_env->empty_logdata_dir();
    sm_options options;
    options.set_int_option("sm_logbufsize", SEG_SIZE);
    options.set_int_option("sm_logsize", LOG_SIZE);
    EXPECT_EQ(0, test_env->runBtreeTest(test_nochange, options));
}

w_rc_t test_one_change(ss_m* ssm, test_volume_t *test_volume) {
    stid_t stid;
    lpid_t root_pid;
    shpid_t target_pid;
    w_keystr_t target_key0, target_key1;
    W_DO (prepare_test(ssm, test_volume, stid, root_pid, target_pid, target_key0, target_key1));
    // After taking backup, remove target_key1, then propagate EMLSN change
    W_DO(ssm->begin_xct());
    W_DO(ssm->destroy_assoc(stid, target_key1));

    // ========================
    rc_t _rc;
    const int BUF_SIZE = 4096*5;
    char _buf[BUF_SIZE];
    memset(_buf, 'z', BUF_SIZE);
    _buf[BUF_SIZE-1] = '\0';
    
    

    for (int i = 0; i < APPEND_COUNT; ++i) {
        _rc = ssm->log_message(_buf);
        EXPECT_FALSE(_rc.is_error()) << "log_message failed?" << _rc;
    }
    // ========================



    W_DO(ssm->commit_xct());
    W_DO(flush_and_evict(ssm));

    corrupt_page(test_volume, target_pid);

    // this should invoke Single-Page-Recovery with one REDO application
    W_DO(ssm->begin_xct());
    char buf[SM_PAGESIZE / 6];
    smsize_t buf_len = SM_PAGESIZE / 6;
    bool found;
    W_DO(ssm->find_assoc(stid, target_key0, buf, buf_len, found));
    EXPECT_TRUE(found);
    EXPECT_EQ((smsize_t)(SM_PAGESIZE / 6), buf_len);
    EXPECT_TRUE(is_consecutive_chars(buf, 'a', SM_PAGESIZE / 6));
    W_DO(ssm->find_assoc(stid, target_key1, buf, buf_len, found));
    EXPECT_FALSE(found);
    W_DO(ssm->commit_xct());

    //Clean up backup file
    BackupManager *bk = ssm->bk;
    volid_t vid = test_volume->_vid;
    x_delete_backup(ssm, test_volume);
    EXPECT_FALSE(bk->volume_exists(vid));
    
    return RCOK;
}
TEST (LogBufferTest_Spr, OneChange) {
    test_env->empty_logdata_dir();
    sm_options options;
    options.set_int_option("sm_logbufsize", SEG_SIZE);
    options.set_int_option("sm_logsize", LOG_SIZE);

    EXPECT_EQ(0, test_env->runBtreeTest(test_one_change, options));
}

w_rc_t test_two_changes(ss_m* ssm, test_volume_t *test_volume) {
    stid_t stid;
    lpid_t root_pid;
    shpid_t target_pid;
    w_keystr_t target_key0, target_key1;
    W_DO (prepare_test(ssm, test_volume, stid, root_pid, target_pid, target_key0, target_key1));
    // After taking backup, remove target_key0/1, then propagate EMLSN change
    W_DO(ssm->begin_xct());
    W_DO(ssm->destroy_assoc(stid, target_key0));
    W_DO(ssm->destroy_assoc(stid, target_key1));

    // ========================
    rc_t _rc;
    const int BUF_SIZE = 4096*5;
    char _buf[BUF_SIZE];
    memset(_buf, 'z', BUF_SIZE);
    _buf[BUF_SIZE-1] = '\0';
    
    

    for (int i = 0; i < APPEND_COUNT; ++i) {
        _rc = ssm->log_message(_buf);
        EXPECT_FALSE(_rc.is_error()) << "log_message failed?" << _rc;
    }
    // ========================





    W_DO(ssm->commit_xct());
    W_DO(flush_and_evict(ssm));

    corrupt_page(test_volume, target_pid);

    // this should invoke Single-Page-Recovery with two REDO applications
    W_DO(ssm->begin_xct());
    char buf[SM_PAGESIZE / 6];
    smsize_t buf_len = SM_PAGESIZE / 6;
    bool found;
    W_DO(ssm->find_assoc(stid, target_key0, buf, buf_len, found));
    EXPECT_FALSE(found);
    W_DO(ssm->find_assoc(stid, target_key1, buf, buf_len, found));
    EXPECT_FALSE(found);
    W_DO(ssm->commit_xct());
    
    //Clean up backup file
    BackupManager *bk = ssm->bk;
    volid_t vid = test_volume->_vid;
    x_delete_backup(ssm, test_volume);
    EXPECT_FALSE(bk->volume_exists(vid));
    
    return RCOK;
}
TEST (LogBufferTest_Spr, TwoChanges) {
    test_env->empty_logdata_dir();
    sm_options options;
    options.set_int_option("sm_logbufsize", SEG_SIZE);
    options.set_int_option("sm_logsize", LOG_SIZE);

    EXPECT_EQ(0, test_env->runBtreeTest(test_two_changes, options));
}

bool test_multi_pages_corrupt_source_page = false;
bool test_multi_pages_corrupt_destination_page = false;
w_rc_t test_multi_pages(ss_m* ssm, test_volume_t *test_volume) {
    stid_t stid;
    lpid_t root_pid;
    shpid_t target_pid;
    w_keystr_t target_key0, target_key1;
    W_DO (prepare_test(ssm, test_volume, stid, root_pid, target_pid, target_key0, target_key1));

    // After taking backup, invoke page split, then propagate EMLSN change
    W_DO(ssm->begin_xct());
    const int recsize = SM_PAGESIZE / 6;
    smsize_t buf_len = recsize;
    char buf[recsize];
    bool found;
    ::memset (buf, 'a', recsize);
    vec_t vec (buf, recsize);

    for (int i = 0; i < 5; ++i) {
        char keystr[7] = "";
        target_key0.serialize_as_nonkeystr(keystr);
        w_keystr_t key;
        keystr[6] = '0' + i;
        key.construct_regularkey(keystr, 7);
        W_DO(ssm->create_assoc(stid, key, vec));
        W_DO(ssm->find_assoc(stid, key, buf, buf_len, found)); // just to invoke adoption
        EXPECT_TRUE(found);
    }
    // this should have caused page split and adoption.
    shpid_t destination_pid = 0; // the new page should be next to target_pid
    {
        btree_page_h root_p;
        W_DO(root_p.fix_root(root_pid.vol().vol, root_pid.store(), LATCH_SH));
        for (int i = 0 ; i < root_p.nrecs(); ++i) {
            if (root_p.child(i) == target_pid) {
                destination_pid = root_p.child(i + 1);
                break;
            }
        }
    }
    std::cout << "multi_pages: destination_pid=" << destination_pid << std::endl;

    // ========================
    rc_t _rc;
    const int BUF_SIZE = 4096*5;
    char _buf[BUF_SIZE];
    memset(_buf, 'z', BUF_SIZE);
    _buf[BUF_SIZE-1] = '\0';
    
    

    for (int i = 0; i < APPEND_COUNT; ++i) {
        _rc = ssm->log_message(_buf);
        EXPECT_FALSE(_rc.is_error()) << "log_message failed?" << _rc;
    }
    // ========================


    W_DO(ssm->commit_xct());
    W_DO(x_btree_verify(ssm, stid));
    W_DO(flush_and_evict(ssm));

    // target_pid is the data source page of split.
    if (test_multi_pages_corrupt_source_page) {
        corrupt_page(test_volume, target_pid);
    }
    if (test_multi_pages_corrupt_destination_page) {
        corrupt_page(test_volume, destination_pid);
    }

    // this should invoke Single-Page-Recovery with multi-page REDO applications (split/rebalance/adopt)
    W_DO(ssm->begin_xct());
    W_DO(ssm->find_assoc(stid, target_key0, buf, buf_len, found));
    EXPECT_TRUE(found);
    EXPECT_EQ((smsize_t)recsize, buf_len);
    EXPECT_TRUE(is_consecutive_chars(buf, 'a', recsize));
    W_DO(ssm->find_assoc(stid, target_key1, buf, buf_len, found));
    EXPECT_TRUE(found);
    EXPECT_EQ((smsize_t)recsize, buf_len);
    EXPECT_TRUE(is_consecutive_chars(buf, 'a', recsize));
    for (int i = 0; i < 5; ++i) {
        char keystr[7] = "";
        target_key0.serialize_as_nonkeystr(keystr);
        w_keystr_t key;
        keystr[6] = '0' + i;
        key.construct_regularkey(keystr, 7);
        W_DO(ssm->find_assoc(stid, key, buf, buf_len, found)); // just to invoke adoption
        EXPECT_TRUE(found);
        EXPECT_EQ((smsize_t)recsize, buf_len);
        EXPECT_TRUE(is_consecutive_chars(buf, 'a', recsize));
    }
    W_DO(ssm->commit_xct());

    //Clean up backup file
    BackupManager *bk = ssm->bk;
    volid_t vid = test_volume->_vid;
    x_delete_backup(ssm, test_volume);
    EXPECT_FALSE(bk->volume_exists(vid));
    
    return RCOK;
}



// which pages to corrupt?
TEST (LogBufferTest_Spr, MultiPagesNone) {
    test_env->empty_logdata_dir();
    test_multi_pages_corrupt_source_page = false;
    test_multi_pages_corrupt_destination_page = false;
    sm_options options;
    options.set_int_option("sm_logbufsize", SEG_SIZE);
    options.set_int_option("sm_logsize", LOG_SIZE);
    EXPECT_EQ(0, test_env->runBtreeTest(test_multi_pages, options));
}
TEST (LogBufferTest_Spr, MultiPagesSourceOnly) {
    test_env->empty_logdata_dir();
    test_multi_pages_corrupt_source_page = true;
    test_multi_pages_corrupt_destination_page = false;
    sm_options options;
    options.set_int_option("sm_logbufsize", SEG_SIZE);
    options.set_int_option("sm_logsize", LOG_SIZE);
    EXPECT_EQ(0, test_env->runBtreeTest(test_multi_pages, options));
}
TEST (LogBufferTest_Spr, MultiPagesDestinationOnly) {
    test_env->empty_logdata_dir();
    test_multi_pages_corrupt_source_page = false;
    test_multi_pages_corrupt_destination_page = true;
    sm_options options;
    options.set_int_option("sm_logbufsize", SEG_SIZE);
    options.set_int_option("sm_logsize", LOG_SIZE);
    EXPECT_EQ(0, test_env->runBtreeTest(test_multi_pages, options));
}
TEST (LogBufferTest_Spr, MultiPagesDestinationBoth) {
    test_env->empty_logdata_dir();
    test_multi_pages_corrupt_source_page = true;
    test_multi_pages_corrupt_destination_page = true;
    sm_options options;
    options.set_int_option("sm_logbufsize", SEG_SIZE);
    options.set_int_option("sm_logsize", LOG_SIZE);
    EXPECT_EQ(0, test_env->runBtreeTest(test_multi_pages, options));
}


int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    test_env = new btree_test_env();
    ::testing::AddGlobalTestEnvironment(test_env);
    return RUN_ALL_TESTS();
}