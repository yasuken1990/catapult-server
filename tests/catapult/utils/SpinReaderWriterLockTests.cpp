#include "catapult/utils/SpinReaderWriterLock.h"
#include "tests/test/nodeps/LockTestUtils.h"
#include "tests/TestHarness.h"

namespace catapult { namespace utils {

	TEST(SpinReaderWriterLockTests, LockIsInitiallyUnlocked) {
		// Act:
		SpinReaderWriterLock lock;

		// Assert:
		EXPECT_FALSE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_FALSE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, CanAcquireReaderLock) {
		// Act:
		SpinReaderWriterLock lock;
		auto readLock = lock.acquireReader();

		// Assert:
		EXPECT_FALSE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_TRUE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, CanReleaseReaderLock) {
		// Act:
		SpinReaderWriterLock lock;
		{
			auto readLock = lock.acquireReader();
		}

		// Assert:
		EXPECT_FALSE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_FALSE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, CanReleaseReaderLockAfterMove) {
		// Act:
		SpinReaderWriterLock lock;
		{
			auto readLock = lock.acquireReader();
			auto readLock2 = std::move(readLock);
		}

		// Assert:
		EXPECT_FALSE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_FALSE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, CanPromoteReaderLockToWriterLock) {
		// Act:
		SpinReaderWriterLock lock;
		auto readLock = lock.acquireReader();
		auto writeLock = readLock.promoteToWriter();

		// Assert:
		EXPECT_TRUE(lock.isWriterPending());
		EXPECT_TRUE(lock.isWriterActive());
		EXPECT_FALSE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, CanDemoteWriterLockToReaderLock) {
		// Act:
		SpinReaderWriterLock lock;
		auto readLock = lock.acquireReader();
		{
			auto writeLock = readLock.promoteToWriter();
		}

		// Assert:
		EXPECT_FALSE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_TRUE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, CanReleaseWriterLock) {
		// Act:
		SpinReaderWriterLock lock;
		{
			auto readLock = lock.acquireReader();
			auto writeLock = readLock.promoteToWriter();
		}

		// Assert:
		EXPECT_FALSE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_FALSE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, CanReleaseWriterLockAfterMove) {
		// Act:
		SpinReaderWriterLock lock;
		{
			auto readLock = lock.acquireReader();
			auto writeLock = readLock.promoteToWriter();
			auto writeLock2 = std::move(writeLock);
		}

		// Assert:
		EXPECT_FALSE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_FALSE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, CannotPromoteReaderLockToWriterLockMultipleTimes) {
		// Act:
		SpinReaderWriterLock lock;
		auto readLock = lock.acquireReader();
		auto writeLock = readLock.promoteToWriter();

		// Assert:
		EXPECT_THROW(readLock.promoteToWriter(), catapult_runtime_error);
	}

	TEST(SpinReaderWriterLockTests, CanPromoteReaderLockToWriterLockAfterDemotion) {
		// Act: acquire a reader and then promote, demote, promote
		SpinReaderWriterLock lock;
		auto readLock = lock.acquireReader();
		{
			auto writeLock = readLock.promoteToWriter();
		}
		auto writeLock = readLock.promoteToWriter();

		// Assert:
		EXPECT_TRUE(lock.isWriterPending());
		EXPECT_TRUE(lock.isWriterActive());
		EXPECT_FALSE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, MultipleThreadsCanAquireReaderLock) {
		// Arrange:
		SpinReaderWriterLock lock;
		std::atomic<uint32_t> counter(0);
		test::LockTestState state;
		test::LockTestGuard testGuard(state);

		for (auto i = 0u; i < test::Num_Default_Lock_Threads; ++i) {
			testGuard.Threads.create_thread([&, i] {
				// Act: acquire a reader and increment the counter
				auto readLock = lock.acquireReader();
				state.incrementCounterAndBlock(counter, i);
			});
		}

		// - wait for the counter to be incremented by all readers
		CATAPULT_LOG(debug) << "waiting for readers";
		WAIT_FOR_VALUE(counter, test::Num_Default_Lock_Threads);

		// Assert: all threads were able to access the counter
		EXPECT_EQ(test::Num_Default_Lock_Threads, counter);
		EXPECT_FALSE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_TRUE(lock.isReaderActive());
	}

	namespace {
		struct ExclusiveLockGuard {
		public:
			explicit ExclusiveLockGuard(SpinReaderWriterLock& lock)
					: m_readLock(lock.acquireReader())
					, m_writeLock(m_readLock.promoteToWriter())
			{}

		private:
			SpinReaderWriterLock::ReaderLockGuard m_readLock;
			SpinReaderWriterLock::WriterLockGuard m_writeLock;
		};

		struct LockPolicy {
			using LockType = SpinReaderWriterLock;

			static auto ExclusiveLock(LockType& lock) {
				return ExclusiveLockGuard(lock);
			}
		};
	}

	TEST(SpinReaderWriterLockTests, LockGuaranteesExclusiveWriterAccess) {
		// Arrange:
		SpinReaderWriterLock lock;

		// Assert:
		test::AssertLockGuaranteesExclusiveAccess<LockPolicy>(lock);
	}

	TEST(SpinReaderWriterLockTests, LockGuaranteesExclusiveWriterAccessAfterLockUnlockCycles) {
		// Arrange:
		SpinReaderWriterLock lock;

		// Assert:
		test::AssertLockGuaranteesExclusiveAccessAfterLockUnlockCycles<LockPolicy>(lock);
	}

	TEST(SpinReaderWriterLockTests, ReaderBlocksWriter) {
		// Arrange:
		SpinReaderWriterLock lock;
		char value = '\0';
		test::LockTestState state;
		test::LockTestGuard testGuard(state);

		// Act: spawn the reader thread
		testGuard.Threads.create_thread([&] {
			// - acquire a reader and then spawn thread that takes a write lock
			auto readLock = lock.acquireReader();
			testGuard.Threads.create_thread([&] {
				// - the writer should be blocked because the outer thread is holding a read lock
				auto readLock2 = lock.acquireReader();
				auto writeLock2 = readLock2.promoteToWriter();
				state.setValueAndBlock(value, 'w');
			});

			state.setValueAndBlock(value, 'r');
		});

		// - wait for the value to be set
		state.waitForValueChangeWithPause();

		// Assert: only the reader was executed
		EXPECT_EQ(1u, state.NumValueChanges);
		EXPECT_EQ('r', value);
		EXPECT_TRUE(lock.isWriterPending());
		EXPECT_FALSE(lock.isWriterActive());
		EXPECT_TRUE(lock.isReaderActive());
	}

	TEST(SpinReaderWriterLockTests, WriterBlocksReader) {
		// Arrange:
		SpinReaderWriterLock lock;
		char value = '\0';
		test::LockTestState state;
		test::LockTestGuard testGuard(state);

		// Act: spawn the writer thread
		testGuard.Threads.create_thread([&] {
			// - acquire a writer and then spawn thread that takes a read lock
			auto readLock = lock.acquireReader();
			auto writeLock = readLock.promoteToWriter();
			testGuard.Threads.create_thread([&] {
				// - the reader should be blocked because the outer thread is holding a write lock
				auto readLock2 = lock.acquireReader();
				state.setValueAndBlock(value, 'r');
			});

			state.setValueAndBlock(value, 'w');
		});

		// - wait for the value to be set
		state.waitForValueChangeWithPause();

		// Assert: only the writer was executed
		EXPECT_EQ(1u, state.NumValueChanges);
		EXPECT_EQ('w', value);
		EXPECT_TRUE(lock.isWriterPending());
		EXPECT_TRUE(lock.isWriterActive());
		EXPECT_FALSE(lock.isReaderActive());
	}

	namespace {
		struct ReaderWriterRaceState : public test::LockTestState {
		public:
			SpinReaderWriterLock Lock;
			std::atomic<char> ReleasedThreadId;
			std::atomic<uint32_t> NumWaitingThreads;
			std::atomic<uint32_t> NumReaderThreads;

		public:
			ReaderWriterRaceState() : ReleasedThreadId('\0'), NumWaitingThreads(0), NumReaderThreads(0)
			{}

		public:
			auto acquireReader() {
				++NumWaitingThreads;
				auto readLock = Lock.acquireReader();
				++NumReaderThreads;
				return readLock;
			}

		public:
			void doWriterWork() {
				doWriterWork(acquireReader());
			}

			void doWriterWork(SpinReaderWriterLock::ReaderLockGuard&& readLock) {
				auto writeLock = readLock.promoteToWriter();

				setReleasedThreadId('w');
				block();
			}

			void doReaderWork() {
				auto readLock = acquireReader();

				setReleasedThreadId('r');
				block();
			}

			void waitForReleasedThread() {
				WAIT_FOR_EXPR('\0' != ReleasedThreadId);
			}

		private:
			void setReleasedThreadId(char ch) {
				char expected = '\0';
				ReleasedThreadId.compare_exchange_strong(expected, ch);
			}
		};
	}

	TEST(SpinReaderWriterLockTests, WriterIsPreferredToReader) {
		// Arrange:
		//  M: |ReadLock     |      # M acquires ReadLock while other threads are spawned
		//  W:   |WriteLock**  |    # when M ReadLock is released, pending writer is unblocked
		//  R:     |ReadLock***  |  # when W WriteLock is released, pending reader2 is unblocked
		ReaderWriterRaceState state;
		test::LockTestGuard testGuard(state);

		// Act: spawn a reader thread
		testGuard.Threads.create_thread([&] {
			// - acquire a reader lock
			auto readLock = state.Lock.acquireReader();

			// - spawn a thread that will acquire a writer lock
			testGuard.Threads.create_thread([&] {
				state.doWriterWork();
			});

			// - spawn a thread that will acquire a reader lock after a writer is pending
			testGuard.Threads.create_thread([&] {
				WAIT_FOR_EXPR(state.Lock.isWriterPending());
				state.doReaderWork();
			});

			// - block until both the reader and writer threads are pending
			WAIT_FOR_VALUE(state.NumWaitingThreads, 2);

			// - wait a bit in case the state changes due to a bug
			test::Pause();
		});

		// - wait for releasedThreadId to be set
		state.waitForReleasedThread();

		// Assert: the writer was released first (the reader was blocked by the pending writer)
		EXPECT_EQ('w', state.ReleasedThreadId);
	}

	TEST(SpinReaderWriterLockTests, WriterIsBlockedByAllPendingReaders) {
		// Arrange:
		//  M: |ReadLock       |        # M acquires ReadLock while other threads are spawned
		//  W:   |ReadLock           |  # when M ReadLock is released, pending reader1 is unblocked
		//  R:     |ReadLock       |    # when M ReadLock is released, pending reader2 is unblocked
		//  W:       [WriteLock****  |  # when R ReadLock is released, pending writer is unblocked
		//                              # (note that promotion is blocked by R ReadLock)
		ReaderWriterRaceState state;
		test::LockTestGuard testGuard(state);

		// Act: spawn a reader thread
		testGuard.Threads.create_thread([&] {
			// Act: acquire a reader lock
			auto readLock = state.Lock.acquireReader();

			// - spawn a thread that will acquire a writer lock after multiple readers (including itself) are active
			testGuard.Threads.create_thread([&] {
				auto writerThreadReadLock = state.acquireReader();
				WAIT_FOR_VALUE(state.NumReaderThreads, 2);
				state.doWriterWork(std::move(writerThreadReadLock));
			});

			// - spawn a thread that will acquire a reader lock after the writer thread
			testGuard.Threads.create_thread([&] {
				WAIT_FOR_VALUE(state.NumReaderThreads, 1);
				state.doReaderWork();
			});

			// - block until both the reader and writer threads have acquired a reader lock
			WAIT_FOR_VALUE(state.NumReaderThreads, 2);

			// - wait a bit in case the state changes due to a bug
			test::Pause();
		});

		// - wait for releasedThreadId to be set
		state.waitForReleasedThread();

		// Assert: the reader was released first (the writer was blocked by the reader)
		EXPECT_EQ('r', state.ReleasedThreadId);
	}
}}