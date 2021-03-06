/**
 *    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_buffer_collection.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

class OplogBufferCollectionTest : public ServiceContextMongoDTest {
protected:
    Client* getClient() const;

protected:
    ServiceContext::UniqueOperationContext makeOperationContext() const;

    ServiceContext::UniqueOperationContext _txn;

private:
    void setUp() override;
    void tearDown() override;
};

void OplogBufferCollectionTest::setUp() {
    ServiceContextMongoDTest::setUp();

    // Initializes cc() used in ServiceContextMongoD::_newOpCtx().
    Client::initThreadIfNotAlready("OplogBufferCollectionTest");

    auto serviceContext = getGlobalServiceContext();

    // AutoGetCollectionForRead requires a valid replication coordinator in order to check the shard
    // version.
    ReplSettings replSettings;
    replSettings.setOplogSizeBytes(5 * 1024 * 1024);
    ReplicationCoordinator::set(serviceContext,
                                stdx::make_unique<ReplicationCoordinatorMock>(replSettings));

    StorageInterface::set(serviceContext, stdx::make_unique<StorageInterfaceImpl>());

    _txn = makeOperationContext();
}

void OplogBufferCollectionTest::tearDown() {
    _txn.reset();

    ServiceContextMongoDTest::tearDown();
}

ServiceContext::UniqueOperationContext OplogBufferCollectionTest::makeOperationContext() const {
    return cc().makeOperationContext();
}

Client* OplogBufferCollectionTest::getClient() const {
    return &cc();
}

/**
 * Generates a unique namespace from the test registration agent.
 */
template <typename T>
NamespaceString makeNamespace(const T& t, const char* suffix = "") {
    return NamespaceString("local." + t.getSuiteName() + "_" + t.getTestName() + suffix);
}

/**
 * Generates oplog entries with the given number used for the timestamp.
 */
BSONObj makeOplogEntry(int t) {
    return BSON("ts" << Timestamp(t, t) << "h" << t << "ns"
                     << "a.a"
                     << "v"
                     << 2
                     << "op"
                     << "i"
                     << "o"
                     << BSON("_id" << t << "a" << t));
}

TEST_F(OplogBufferCollectionTest, DefaultNamespace) {
    ASSERT_EQUALS(OplogBufferCollection::getDefaultNamespace(),
                  OplogBufferCollection().getNamespace());
}

TEST_F(OplogBufferCollectionTest, GetNamespace) {
    auto nss = makeNamespace(_agent);
    ASSERT_EQUALS(nss, OplogBufferCollection(nss).getNamespace());
}

TEST_F(OplogBufferCollectionTest, StartupCreatesCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    // Collection should not exist until startup() is called.
    ASSERT_FALSE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());

    oplogBuffer.startup(_txn.get());
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
}

TEST_F(OplogBufferCollectionTest, ShutdownDropsCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
    oplogBuffer.shutdown(_txn.get());
    ASSERT_FALSE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
}

TEST_F(OplogBufferCollectionTest, extractEmbeddedOplogDocumentChangesIdToTimestamp) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    const BSONObj expectedOp = makeOplogEntry(1);
    BSONObj originalOp = BSON("_id" << Timestamp(1, 1) << "entry" << expectedOp);
    ASSERT_EQUALS(expectedOp, OplogBufferCollection::extractEmbeddedOplogDocument(originalOp));
}

TEST_F(OplogBufferCollectionTest, addIdToDocumentChangesTimestampToId) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    const BSONObj originalOp = makeOplogEntry(1);
    BSONObj expectedOp = BSON("_id" << Timestamp(1, 1) << "entry" << originalOp);
    ASSERT_EQUALS(expectedOp, OplogBufferCollection::addIdToDocument(originalOp));
}

#if 0

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushAllNonBlockingAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {makeOplogEntry(1)};
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_TRUE(oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end()));
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[0],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog,
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PushOneDocumentWithPushEvenIfFullAddsDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog,
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PeekDoesNotRemoveDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog,
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PopRemovesDocument) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
    }
}

TEST_F(OplogBufferCollectionTest, PopAndPeekReturnDocumentsInOrder) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(2), makeOplogEntry(1), makeOplogEntry(3),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_TRUE(oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end()));
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(oplog[2],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[1],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(oplog[0],
                      OplogBufferCollection::extractEmbeddedOplogDocument(
                          unittest::assertGet(iter->next()).first));
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
    }

    BSONObj doc;
    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 2UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[0]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    ASSERT_TRUE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_EQUALS(doc, oplog[2]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
}

TEST_F(OplogBufferCollectionTest, LastObjectPushedReturnsNewestOplogEntry) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    const std::vector<BSONObj> oplog = {
        makeOplogEntry(1), makeOplogEntry(3), makeOplogEntry(2),
    };
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    ASSERT_TRUE(oplogBuffer.pushAllNonBlocking(_txn.get(), oplog.begin(), oplog.end()));
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);

    auto doc = oplogBuffer.lastObjectPushed(_txn.get());
    ASSERT_EQUALS(*doc, oplog[1]);
    ASSERT_EQUALS(oplogBuffer.getCount(), 3UL);
}

TEST_F(OplogBufferCollectionTest, LastObjectPushedReturnsNoneWithNoEntries) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());

    auto doc = oplogBuffer.lastObjectPushed(_txn.get());
    ASSERT_FALSE(doc);
}

TEST_F(OplogBufferCollectionTest, IsEmptyReturnsTrueWhenEmptyAndFalseWhenNot) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_TRUE(oplogBuffer.isEmpty());
    oplogBuffer.pushEvenIfFull(_txn.get(), oplog);
    ASSERT_FALSE(oplogBuffer.isEmpty());
}

TEST_F(OplogBufferCollectionTest, ClearClearsCollection) {
    auto nss = makeNamespace(_agent);
    OplogBufferCollection oplogBuffer(nss);

    oplogBuffer.startup(_txn.get());
    BSONObj oplog = makeOplogEntry(1);
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);
    oplogBuffer.push(_txn.get(), oplog);
    ASSERT_EQUALS(oplogBuffer.getCount(), 1UL);

    oplogBuffer.clear(_txn.get());
    ASSERT_TRUE(AutoGetCollectionForRead(_txn.get(), nss).getCollection());
    ASSERT_EQUALS(oplogBuffer.getCount(), 0UL);

    {
        OplogInterfaceLocal collectionReader(_txn.get(), nss.ns());
        auto iter = collectionReader.makeIterator();
        ASSERT_EQUALS(ErrorCodes::NoSuchKey, iter->next().getStatus());
    }

    BSONObj doc;
    ASSERT_FALSE(oplogBuffer.peek(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
    ASSERT_FALSE(oplogBuffer.tryPop(_txn.get(), &doc));
    ASSERT_TRUE(doc.isEmpty());
}
#endif

}  // namespace
