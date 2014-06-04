// Copyright (c) 2013, Cloudera, inc.
// All rights reserved.

#include <boost/thread/thread.hpp>
#include <gtest/gtest.h>
#include <glog/logging.h>

#include "server/logical_clock.h"
#include "tablet/mvcc.h"
#include "util/test_util.h"

namespace kudu { namespace tablet {

class MvccTest : public KuduTest {
 public:
  MvccTest() :
    clock_(server::LogicalClock::CreateStartingAt(Timestamp::kInitialTimestamp)) {
  }

  void WaitForSnapshotAtTSThread(MvccManager* mgr, Timestamp ts) {
    MvccSnapshot s;
    mgr->WaitForCleanSnapshotAtTimestamp(ts, &s);
    mgr->WaitUntilAllCommitted(ts);
    CHECK(s.is_clean()) << "verifying postcondition";
    boost::lock_guard<simple_spinlock> lock(lock_);
    result_snapshot_.reset(new MvccSnapshot(s));
  }

  bool HasResultSnapshot() {
    boost::lock_guard<simple_spinlock> lock(lock_);
    return result_snapshot_ != NULL;
  }

 protected:
  scoped_refptr<server::Clock> clock_;

  mutable simple_spinlock lock_;
  gscoped_ptr<MvccSnapshot> result_snapshot_;
};

TEST_F(MvccTest, TestMvccBasic) {
  MvccManager mgr(clock_.get());
  MvccSnapshot snap;

  // Initial state should not have any committed transactions.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(Timestamp(1)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(2)));

  // Start timestamp 1
  Timestamp t = mgr.StartTransaction();
  ASSERT_EQ(1, t.value());

  // State should still have no committed transactions, since 1 is in-flight.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1 or (T < 2 and T not in {1})}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(Timestamp(1)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(2)));

  // Commit timestamp 1
  mgr.CommitTransaction(t);

  // State should show 0 as committed, 1 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 2}]", snap.ToString());
  ASSERT_TRUE(snap.IsCommitted(Timestamp(1)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(2)));
}

TEST_F(MvccTest, TestMvccMultipleInFlight) {
  MvccManager mgr(clock_.get());
  MvccSnapshot snap;

  // Start timestamp 1, timestamp 2
  Timestamp t1 = mgr.StartTransaction();
  ASSERT_EQ(1, t1.value());
  Timestamp t2 = mgr.StartTransaction();
  ASSERT_EQ(2, t2.value());

  // State should still have no committed transactions, since both are in-flight.

  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1 or (T < 3 and T not in {1,2})}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_FALSE(snap.IsCommitted(t2));

  // Commit timestamp 2
  mgr.CommitTransaction(t2);

  // State should show 1 as committed, 0 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T < 3 and T not in {1})}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));

  // Start timestamp 3
  Timestamp t3 = mgr.StartTransaction();
  ASSERT_EQ(3, t3.value());

  // State should show 2 as committed, 1 and 3 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T < 4 and T not in {1,3})}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_FALSE(snap.IsCommitted(t3));

  // Commit 3
  mgr.CommitTransaction(t3);

  // 2 and 3 committed
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T < 4 and T not in {1})}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_TRUE(snap.IsCommitted(t3));

  // Commit 1
  mgr.CommitTransaction(t1);

  // all committed
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 4}]", snap.ToString());
  ASSERT_TRUE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_TRUE(snap.IsCommitted(t3));
}

TEST_F(MvccTest, TestScopedTransaction) {
  MvccManager mgr(clock_.get());
  MvccSnapshot snap;

  {
    ScopedTransaction t1(&mgr);
    ScopedTransaction t2(&mgr);

    ASSERT_EQ(1, t1.timestamp().value());
    ASSERT_EQ(2, t2.timestamp().value());

    t1.Commit();

    mgr.TakeSnapshot(&snap);
    ASSERT_TRUE(snap.IsCommitted(t1.timestamp()));
    ASSERT_FALSE(snap.IsCommitted(t2.timestamp()));
  }

  // t2 going out of scope commits it.
  mgr.TakeSnapshot(&snap);
  ASSERT_TRUE(snap.IsCommitted(Timestamp(1)));
  ASSERT_TRUE(snap.IsCommitted(Timestamp(2)));
}

TEST_F(MvccTest, TestPointInTimeSnapshot) {
  MvccSnapshot snap(Timestamp(10));

  ASSERT_TRUE(snap.IsCommitted(Timestamp(1)));
  ASSERT_TRUE(snap.IsCommitted(Timestamp(9)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(10)));
  ASSERT_FALSE(snap.IsCommitted(Timestamp(11)));
}

TEST_F(MvccTest, TestMayHaveCommittedTransactionsAtOrAfter) {
  MvccSnapshot snap;
  snap.all_committed_before_timestamp_ = Timestamp(10);
  snap.timestamps_in_flight_.insert(11);
  snap.timestamps_in_flight_.insert(13);
  snap.none_committed_after_timestamp_ = Timestamp(14);

  ASSERT_TRUE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(9)));
  ASSERT_TRUE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(10)));
  ASSERT_TRUE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(12)));
  ASSERT_TRUE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(13)));
  ASSERT_FALSE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(14)));
  ASSERT_FALSE(snap.MayHaveCommittedTransactionsAtOrAfter(Timestamp(15)));
}

TEST_F(MvccTest, TestMayHaveUncommittedTransactionsBefore) {
  MvccSnapshot snap;
  snap.all_committed_before_timestamp_ = Timestamp(10);
  snap.timestamps_in_flight_.insert(11);
  snap.timestamps_in_flight_.insert(13);
  snap.none_committed_after_timestamp_ = Timestamp(14);

  ASSERT_FALSE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(9)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(10)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(11)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(13)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(14)));
  ASSERT_TRUE(snap.MayHaveUncommittedTransactionsAtOrBefore(Timestamp(15)));
}

TEST_F(MvccTest, TestAreAllTransactionsCommitted) {
  MvccManager mgr(clock_.get());

  // start several transactions and take snapshots along the way
  Timestamp tx1 = mgr.StartTransaction();
  Timestamp tx2 = mgr.StartTransaction();
  Timestamp tx3 = mgr.StartTransaction();

  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(1)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(2)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(3)));

  // commit tx3, should all still report as having as having uncommitted
  // transactions.
  mgr.CommitTransaction(tx3);
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(1)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(2)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(3)));

  // commit tx1, first snap with in-flights should now report as all committed
  // and remaining snaps as still having uncommitted transactions
  mgr.CommitTransaction(tx1);
  ASSERT_TRUE(mgr.AreAllTransactionsCommitted(Timestamp(1)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(2)));
  ASSERT_FALSE(mgr.AreAllTransactionsCommitted(Timestamp(3)));

  // Now they should all report as all committed.
  mgr.CommitTransaction(tx2);
  ASSERT_TRUE(mgr.AreAllTransactionsCommitted(Timestamp(1)));
  ASSERT_TRUE(mgr.AreAllTransactionsCommitted(Timestamp(2)));
  ASSERT_TRUE(mgr.AreAllTransactionsCommitted(Timestamp(3)));
}

TEST_F(MvccTest, TestWaitUntilAllCommitted_SnapWithNoInflights) {
  MvccManager mgr(clock_.get());
  boost::thread waiting_thread = boost::thread(&MvccTest::WaitForSnapshotAtTSThread,
                                               this,
                                               &mgr,
                                               clock_->Now());

  // join immediately.
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

TEST_F(MvccTest, TestWaitUntilAllCommitted_SnapWithInFlights) {

  MvccManager mgr(clock_.get());

  Timestamp tx1 = mgr.StartTransaction();
  Timestamp tx2 = mgr.StartTransaction();

  boost::thread waiting_thread = boost::thread(&MvccTest::WaitForSnapshotAtTSThread,
                                               this,
                                               &mgr,
                                               clock_->Now());

  ASSERT_FALSE(HasResultSnapshot());
  mgr.CommitTransaction(tx1);
  ASSERT_FALSE(HasResultSnapshot());
  mgr.CommitTransaction(tx2);
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

TEST_F(MvccTest, TestWaitUntilAllCommitted_SnapAtTimestampWithInFlights) {

  MvccManager mgr(clock_.get());

  // Transactions with timestamp 1 through 3
  Timestamp tx1 = mgr.StartTransaction();
  Timestamp tx2 = mgr.StartTransaction();
  Timestamp tx3 = mgr.StartTransaction();

  // Start a thread waiting for transactions with ts <= 2 to commit
  boost::thread waiting_thread = boost::thread(&MvccTest::WaitForSnapshotAtTSThread,
                                               this,
                                               &mgr,
                                               tx2);
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 1 - thread should still wait.
  mgr.CommitTransaction(tx1);
  usleep(1000);
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 3 - thread should still wait.
  mgr.CommitTransaction(tx3);
  usleep(1000);
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 2 - thread can now continue
  mgr.CommitTransaction(tx2);
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

} // namespace tablet
} // namespace kudu
