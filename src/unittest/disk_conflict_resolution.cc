// Copyright 2010-2012 RethinkDB, all rights reserved.
#include <vector>

#include "errors.hpp"
#include <boost/bind.hpp>
#include <boost/ptr_container/ptr_vector.hpp>

#include "arch/io/disk/conflict_resolving.hpp"
#include "arch/io/disk/accounting.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "containers/intrusive_list.hpp"
#include "containers/scoped.hpp"
#include "unittest/gtest.hpp"

namespace unittest {

/* We need for multiple test_driver_t objects to share a file
   descriptor in order to test the conflict resolution logic, but
   it doesn't matter what that file descriptor is. */
static const int IRRELEVANT_DEFAULT_FD = 0;

struct core_action_t : public intrusive_list_node_t<core_action_t>, public accounting_diskmgr_action_t { };

void debug_print(printf_buffer_t *buf,
                 const core_action_t &action) {
    buf->appendf("core_action{is_read=%s, buf=%p, count=%zu, "
                 "offset=%" PRIi64 ", fd=%d}",
                 action.get_is_read() ? "true" : "false",
                 action.get_buf(),
                 action.get_count(),
                 action.get_offset(),
                 action.get_fd());
}

struct test_driver_t {
    typedef conflict_resolving_diskmgr_t<core_action_t>::action_t action_t;

    // We avoid deallocating actions during the test to make sure that each action
    // has a unique pointer value.
    boost::ptr_vector<action_t> allocated_actions;

    intrusive_list_t<core_action_t> running_actions;
    std::vector<char> data;

    conflict_resolving_diskmgr_t<core_action_t> conflict_resolver;

    // These work because all actions are part of allocated_actions -- they have
    // unique pointer values.
    std::set<core_action_t *> actions_that_have_begun;
    std::set<core_action_t *> actions_that_are_done;

    int old_thread_id;
    test_driver_t() : conflict_resolver(&get_global_perfmon_collection()) {
        /* Fake thread-context to make perfmons work. */
        old_thread_id = linux_thread_pool_t::thread_id;
        linux_thread_pool_t::thread_id = 0;

        conflict_resolver.submit_fun = boost::bind(
            &test_driver_t::submit_from_conflict_resolving_diskmgr, this, _1);
        conflict_resolver.done_fun = boost::bind(
            &test_driver_t::done_from_conflict_resolving_diskmgr, this, _1);
    }
    ~test_driver_t() {
        linux_thread_pool_t::thread_id = old_thread_id;
    }

    void submit(action_t *a) {
        conflict_resolver.submit(a);
    }

    action_t *make_action() {
        action_t *ret = new action_t;
        allocated_actions.push_back(ret);
        return ret;
    }

    bool action_has_begun(core_action_t *action) const {
        return actions_that_have_begun.find(action) != actions_that_have_begun.end();
    }

    bool action_is_done(core_action_t *action) const {
        return actions_that_are_done.find(action) != actions_that_are_done.end();
    }

    void submit_from_conflict_resolving_diskmgr(core_action_t *a) {

        rassert(!action_has_begun(a));
        rassert(!action_is_done(a));
        actions_that_have_begun.insert(a);

        /* The conflict_resolving_diskmgr_t should not have sent us two potentially
        conflicting actions */
        for (core_action_t *p = running_actions.head(); p; p = running_actions.next(p)) {
            if (!(a->get_is_read() && p->get_is_read())) {
                ASSERT_TRUE(a->get_offset() >= static_cast<int64_t>(p->get_offset() + p->get_count())
                            || p->get_offset() >= static_cast<int64_t>(a->get_offset() + a->get_count()));
            }
        }

        running_actions.push_back(a);
    }

    void permit(core_action_t *a) {
        if (action_is_done(a)) {
            return;
        }
        rassert(action_has_begun(a));
        running_actions.remove(a);

        if (a->get_offset() + a->get_count() > data.size()) {
            data.resize(a->get_offset() + a->get_count(), 0);
        }
        if (a->get_is_read()) {
            memcpy(a->get_buf(), data.data() + a->get_offset(), a->get_count());
        } else {
            memcpy(data.data() + a->get_offset(), a->get_buf(), a->get_count());
        }

        conflict_resolver.done(a);
    }

    void done_from_conflict_resolving_diskmgr(core_action_t *a) {
        actions_that_are_done.insert(a);
    }
};

struct read_test_t {

    read_test_t(test_driver_t *_driver, int64_t o, const std::string &e) :
        driver(_driver),
        offset(o),
        expected(e),
        buffer(expected.size()),
        action(driver->make_action()) {
        action->make_read(IRRELEVANT_DEFAULT_FD, buffer.data(), expected.size(), offset);
        driver->submit(action);
    }
    test_driver_t *driver;
    int64_t offset;
    std::string expected;
    scoped_array_t<char> buffer;
    test_driver_t::action_t *action;
    bool was_sent() {
        return driver->action_is_done(action) || driver->action_has_begun(action);
    }
    bool was_completed() {
        return driver->action_is_done(action);
    }
    void go() {
        ASSERT_TRUE(was_sent());
        driver->permit(action);
        ASSERT_TRUE(was_completed());
    }
    ~read_test_t() {
        EXPECT_TRUE(was_completed());
        std::string got(buffer.data(), expected.size());
        EXPECT_EQ(expected, got) << "Read returned wrong data.";
    }
};

struct write_test_t {

    write_test_t(test_driver_t *_driver, int64_t o, const std::string &d) :
        driver(_driver),
        offset(o),
        data(d.begin(), d.end()),
        action(driver->make_action()) {
        action->make_write(IRRELEVANT_DEFAULT_FD, data.data(), d.size(), o, false);
        driver->submit(action);
    }

    test_driver_t *driver;
    int64_t offset;
    std::vector<char> data;
    test_driver_t::action_t *action;

    bool was_sent() {
        return driver->action_is_done(action) || driver->action_has_begun(action);
    }
    bool was_completed() {
        return driver->action_is_done(action);
    }
    void go() {
        ASSERT_TRUE(was_sent());
        driver->permit(action);
        ASSERT_TRUE(was_completed());
    }
    ~write_test_t() {
        EXPECT_TRUE(was_completed());
    }
};

/* WriteWriteConflict verifies that if two writes are sent, they will be run in the correct
order. */

TEST(DiskConflictTest, WriteWriteConflict) {
    test_driver_t d;
    write_test_t w1(&d, 0, "foo");
    write_test_t w2(&d, 0, "bar");
    read_test_t verifier(&d, 0, "bar");
    w1.go();
    w2.go();
    verifier.go();
}

/* WriteReadConflict verifies that if a write and then a read are sent, the write will happen
before the read. */

TEST(DiskConflictTest, WriteReadConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "initial");
    write_test_t w(&d, 0, "foo");
    read_test_t r(&d, 0, "foo");
    initial_write.go();
    w.go();
    r.go();
}

/* ReadWriteConflict verifies that if a read and then a write are sent, the read will happen
before the write. */

TEST(DiskConflictTest, ReadWriteConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "initial");
    read_test_t r(&d, 0, "init");
    write_test_t w(&d, 0, "something_else");
    initial_write.go();
    r.go();
    w.go();
}

/* NoSpuriousConflicts verifies that if two writes that don't overlap are sent, there are
no problems. */

TEST(DiskConflictTest, NoSpuriousConflicts) {
    test_driver_t d;
    write_test_t w1(&d, 0, "foo");
    write_test_t w2(&d, 4096, "bar");
    ASSERT_TRUE(w1.was_sent());
    ASSERT_TRUE(w2.was_sent());
    w1.go();
    w2.go();
}

/* ReadReadPass verifies that reads do not block reads */

TEST(DiskConflictTest, NoReadReadConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "foo");
    read_test_t r1(&d, 0, "foo");
    read_test_t r2(&d, 0, "foo");
    initial_write.go();
    ASSERT_TRUE(r1.was_sent());
    ASSERT_TRUE(r2.was_sent());
    r1.go();
    r2.go();
}

/* WriteReadSubrange verifies that if a write and then a read are sent, and the read
is for a subrange of the write, the read gets the right value */

TEST(DiskConflictTest, WriteReadSubrange) {
    test_driver_t d;
    write_test_t w(&d, 0, "abcdefghijklmnopqrstuvwxyz");
    read_test_t r(&d, 3, "defghijkl");
    w.go();
    r.go();
}

/* WriteReadSuperrange verifies that if a write and then a read are sent, and the read
is for a superrange of the write, the read gets the right value */

TEST(DiskConflictTest, WriteReadSuperrange) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "abc____________________xyz");
    write_test_t w(&d, 3, "defghijklmnopqrstuvw");
    read_test_t r(&d, 0, "abcdefghijklmnopqrstuvwxyz");
    initial_write.go();
    w.go();
    r.go();
}

/* MetaTest is a sanity check to make sure that the above tests are actually testing something. */

void cause_test_failure() {
    test_driver_t d;
    write_test_t w(&d, 0, "foo");
    read_test_t r(&d, 0, "bar");   // We write "foo" but expect to read "bar"
    w.go();
    r.go();
}

TEST(DiskConflictTest, MetaTest) {
    EXPECT_NONFATAL_FAILURE(cause_test_failure(), "Read returned wrong data.");
};

}  // namespace unittest

