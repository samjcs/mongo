/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/optional/optional_io.hpp>
#include <iostream>
#include <string>

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/tenant_namespace.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

static std::string kSideWritesTableIdent("sideWrites");
static std::string kConstraintViolationsTableIdent("constraintViolations");

// Update version as breaking changes are introduced into the index build procedure.
static const long kExpectedVersion = 1;

class DurableCatalogTest : public CatalogTestFixture {
public:
    void setUp() final {
        CatalogTestFixture::setUp();

        _tenantNs = TenantNamespace(boost::none, NamespaceString("unittests.durable_catalog"));
        _collectionUUID = createCollection(_tenantNs, CollectionOptions());
    }

    TenantNamespace tenantNs() {
        return _tenantNs;
    }

    DurableCatalog* getCatalog() {
        return operationContext()->getServiceContext()->getStorageEngine()->getCatalog();
    }

    CollectionPtr getCollection() {
        return CollectionCatalog::get(operationContext())
            ->lookupCollectionByUUID(operationContext(), *_collectionUUID);
    }

    CollectionWriter getCollectionWriter() {
        return CollectionWriter(
            operationContext(), *_collectionUUID, CollectionCatalog::LifetimeMode::kInplace);
    }

    UUID createCollection(const TenantNamespace& tenantNs, CollectionOptions options) {
        Lock::DBLock dbLk(operationContext(), tenantNs.getNss().db(), MODE_IX);
        Lock::CollectionLock collLk(operationContext(), tenantNs.getNss(), MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        const bool allocateDefaultSpace = true;
        options.uuid = UUID::gen();

        auto swColl = getCatalog()->createCollection(
            operationContext(), tenantNs, options, allocateDefaultSpace);
        ASSERT_OK(swColl.getStatus());

        std::pair<RecordId, std::unique_ptr<RecordStore>> coll = std::move(swColl.getValue());
        RecordId catalogId = coll.first;

        std::shared_ptr<Collection> collection = std::make_shared<CollectionImpl>(
            operationContext(),
            tenantNs,
            catalogId,
            getCatalog()->getMetaData(operationContext(), catalogId),
            std::move(coll.second));
        CollectionCatalog::write(operationContext(), [&](CollectionCatalog& catalog) {
            catalog.registerCollection(
                operationContext(), options.uuid.get(), std::move(collection));
        });

        wuow.commit();

        return *options.uuid;
    }

    IndexCatalogEntry* createIndex(BSONObj keyPattern,
                                   std::string indexType = IndexNames::BTREE,
                                   bool twoPhase = false) {
        Lock::DBLock dbLk(operationContext(), _tenantNs.getNss().db(), MODE_IX);
        Lock::CollectionLock collLk(operationContext(), _tenantNs.getNss(), MODE_X);

        std::string indexName = "idx" + std::to_string(_numIndexesCreated);
        // Make sure we have a valid IndexSpec for the type requested
        IndexSpec spec;
        spec.version(1).name(indexName).addKeys(keyPattern);
        if (indexType == IndexNames::TEXT) {
            spec.textWeights(BSON("a" << 1));
            spec.textIndexVersion(2);
            spec.textDefaultLanguage("swedish");
        }

        auto desc = std::make_unique<IndexDescriptor>(indexType, spec.toBSON());

        IndexCatalogEntry* entry = nullptr;
        auto collWriter = getCollectionWriter();
        {
            WriteUnitOfWork wuow(operationContext());
            const bool isSecondaryBackgroundIndexBuild = false;
            boost::optional<UUID> buildUUID(twoPhase, UUID::gen());
            ASSERT_OK(collWriter.getWritableCollection()->prepareForIndexBuild(
                operationContext(), desc.get(), buildUUID, isSecondaryBackgroundIndexBuild));
            entry = collWriter.getWritableCollection()->getIndexCatalog()->createIndexEntry(
                operationContext(),
                collWriter.getWritableCollection(),
                std::move(desc),
                CreateIndexEntryFlags::kNone);
            wuow.commit();
        }

        ++_numIndexesCreated;
        return entry;
    }

    void assertMultikeyPathsAreEqual(const MultikeyPaths& actual, const MultikeyPaths& expected) {
        bool match = (expected == actual);
        if (!match) {
            FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expected) << ", "
                               << "Actual: " << dumpMultikeyPaths(actual));
        }
        ASSERT(match);
    }

    StatusWith<DurableCatalog::ImportResult> importCollectionTest(const TenantNamespace& tenantNs,
                                                                  const BSONObj& metadata) {
        Lock::DBLock dbLock(operationContext(), tenantNs.getNss().db(), MODE_IX);
        Lock::CollectionLock collLock(operationContext(), tenantNs.getNss(), MODE_X);

        WriteUnitOfWork wuow(operationContext());
        auto res = getCatalog()->importCollection(
            operationContext(),
            tenantNs,
            metadata,
            BSON("storage"
                 << "metadata"),
            ImportOptions(ImportOptions::ImportCollectionUUIDOption::kGenerateNew));
        if (res.isOK()) {
            wuow.commit();
        }
        return res;
    }

private:
    std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
        std::stringstream ss;

        ss << "[ ";
        for (const auto& multikeyComponents : multikeyPaths) {
            ss << "[ ";
            for (const auto& multikeyComponent : multikeyComponents) {
                ss << multikeyComponent << " ";
            }
            ss << "] ";
        }
        ss << "]";

        return ss.str();
    }

    TenantNamespace _tenantNs;

    size_t _numIndexesCreated = 0;

    boost::optional<UUID> _collectionUUID;
};

TEST_F(DurableCatalogTest, MultikeyPathsForBtreeIndexInitializedToVectorOfEmptySets) {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();
    {
        MultikeyPaths multikeyPaths;
        ASSERT(!collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, MultikeyComponents{}});
    }
}

TEST_F(DurableCatalogTest, CanSetIndividualPathComponentOfBtreeIndexAsMultikey) {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(operationContext(),
                                              indexEntry->descriptor()->indexName(),
                                              {MultikeyComponents{}, {0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, {0U}});
    }
}

TEST_F(DurableCatalogTest, MultikeyPathsAccumulateOnDifferentFields) {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(operationContext(),
                                              indexEntry->descriptor()->indexName(),
                                              {MultikeyComponents{}, {0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, {0U}});
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(operationContext(),
                                              indexEntry->descriptor()->indexName(),
                                              {{0U}, MultikeyComponents{}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}, {0U}});
    }
}

TEST_F(DurableCatalogTest, MultikeyPathsAccumulateOnDifferentComponentsOfTheSameField) {
    auto indexEntry = createIndex(BSON("a.b" << 1));
    auto collection = getCollection();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{1U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U, 1U}});
    }
}

TEST_F(DurableCatalogTest, NoOpWhenSpecifiedPathComponentsAlreadySetAsMultikey) {
    auto indexEntry = createIndex(BSON("a" << 1));
    auto collection = getCollection();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{0U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(!collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{0U}}));
        // Rollback WUOW.
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U}});
    }
}

TEST_F(DurableCatalogTest, CanSetMultipleFieldsAndComponentsAsMultikey) {
    auto indexEntry = createIndex(BSON("a.b.c" << 1 << "a.b.d" << 1));
    auto collection = getCollection();
    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), {{0U, 1U}, {0U, 1U}}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {{0U, 1U}, {0U, 1U}});
    }
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   CannotOmitPathLevelMultikeyInfoWithBtreeIndex,
                   R"#(Invariant failure.*!multikeyPaths.empty\(\))#") {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();

    WriteUnitOfWork wuow(operationContext());
    collection->setIndexIsMultikey(
        operationContext(), indexEntry->descriptor()->indexName(), MultikeyPaths{});
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   AtLeastOnePathComponentMustCauseIndexToBeMultikey,
                   R"#(Invariant failure.*somePathIsMultikey)#") {
    auto indexEntry = createIndex(BSON("a" << 1 << "b" << 1));
    auto collection = getCollection();

    WriteUnitOfWork wuow(operationContext());
    collection->setIndexIsMultikey(operationContext(),

                                   indexEntry->descriptor()->indexName(),
                                   {MultikeyComponents{}, MultikeyComponents{}});
}

TEST_F(DurableCatalogTest, PathLevelMultikeyTrackingIsSupportedBy2dsphereIndexes) {
    std::string indexType = IndexNames::GEO_2DSPHERE;
    auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    auto collection = getCollection();
    {
        MultikeyPaths multikeyPaths;
        ASSERT(!collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        assertMultikeyPathsAreEqual(multikeyPaths, {MultikeyComponents{}, MultikeyComponents{}});
    }
}

TEST_F(DurableCatalogTest, PathLevelMultikeyTrackingIsNotSupportedByAllIndexTypes) {
    std::string indexTypes[] = {IndexNames::GEO_2D, IndexNames::TEXT, IndexNames::HASHED};

    for (auto&& indexType : indexTypes) {
        auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
        auto collection = getCollection();
        {
            MultikeyPaths multikeyPaths;
            ASSERT(!collection->isIndexMultikey(
                operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
            ASSERT(multikeyPaths.empty());
        }
    }
}

TEST_F(DurableCatalogTest, CanSetEntireTextIndexAsMultikey) {
    std::string indexType = IndexNames::TEXT;
    auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    auto collection = getCollection();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), MultikeyPaths{}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }
}

TEST_F(DurableCatalogTest, NoOpWhenEntireIndexAlreadySetAsMultikey) {
    std::string indexType = IndexNames::TEXT;
    auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    auto collection = getCollection();

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), MultikeyPaths{}));
        wuow.commit();
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }

    {
        WriteUnitOfWork wuow(operationContext());
        ASSERT(!collection->setIndexIsMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), MultikeyPaths{}));
        // Rollback WUOW.
    }

    {
        MultikeyPaths multikeyPaths;
        ASSERT(collection->isIndexMultikey(
            operationContext(), indexEntry->descriptor()->indexName(), &multikeyPaths));
        ASSERT(multikeyPaths.empty());
    }
}

TEST_F(DurableCatalogTest, SinglePhaseIndexBuild) {
    auto indexEntry = createIndex(BSON("a" << 1));
    auto collection = getCollection();

    ASSERT_FALSE(collection->isIndexReady(indexEntry->descriptor()->indexName()));
    ASSERT_FALSE(collection->getIndexBuildUUID(indexEntry->descriptor()->indexName()));

    {
        WriteUnitOfWork wuow(operationContext());
        getCollectionWriter().getWritableCollection()->indexBuildSuccess(operationContext(),
                                                                         indexEntry);
        wuow.commit();
    }

    collection = getCollection();
    ASSERT_TRUE(collection->isIndexReady(indexEntry->descriptor()->indexName()));
    ASSERT_FALSE(collection->getIndexBuildUUID(indexEntry->descriptor()->indexName()));
}

TEST_F(DurableCatalogTest, TwoPhaseIndexBuild) {
    bool twoPhase = true;
    auto indexEntry = createIndex(BSON("a" << 1), IndexNames::BTREE, twoPhase);
    auto collection = getCollection();

    ASSERT_FALSE(collection->isIndexReady(indexEntry->descriptor()->indexName()));
    ASSERT_TRUE(collection->getIndexBuildUUID(indexEntry->descriptor()->indexName()));

    {
        WriteUnitOfWork wuow(operationContext());
        getCollectionWriter().getWritableCollection()->indexBuildSuccess(operationContext(),
                                                                         indexEntry);
        wuow.commit();
    }

    collection = getCollection();
    ASSERT_TRUE(collection->isIndexReady(indexEntry->descriptor()->indexName()));
    ASSERT_FALSE(collection->getIndexBuildUUID(indexEntry->descriptor()->indexName()));
}

DEATH_TEST_REGEX_F(DurableCatalogTest,
                   CannotSetIndividualPathComponentsOfTextIndexAsMultikey,
                   R"#(Invariant failure.*multikeyPaths.empty\(\))#") {
    std::string indexType = IndexNames::TEXT;
    auto indexEntry = createIndex(BSON("a" << indexType << "b" << 1), indexType);
    auto collection = getCollection();

    WriteUnitOfWork wuow(operationContext());
    collection->setIndexIsMultikey(
        operationContext(), indexEntry->descriptor()->indexName(), {{0U}, {0U}});
}

TEST_F(DurableCatalogTest, ImportCollection) {
    // Import should fail if the namespace already exists.
    ASSERT_THROWS_CODE(
        importCollectionTest(tenantNs(), {}), AssertionException, ErrorCodes::NamespaceExists);

    const auto tenantNs = TenantNamespace(boost::none, NamespaceString("unittest.import"));

    // Import should fail with empty metadata.
    ASSERT_THROWS_CODE(
        importCollectionTest(tenantNs, {}), AssertionException, ErrorCodes::BadValue);

    BSONCollectionCatalogEntry::MetaData md;

    md.tenantNs = tenantNs;

    CollectionOptions optionsWithUUID;
    optionsWithUUID.uuid = UUID::gen();
    md.options = optionsWithUUID;

    BSONCollectionCatalogEntry::IndexMetaData indexMetaData;
    indexMetaData.spec = BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                  << "_id_");
    indexMetaData.ready = true;
    md.indexes.push_back(indexMetaData);

    auto mdObj = md.toBSON();
    const auto ident = "collection-7-1792004489479993697";
    const auto idxIdent = "index-8-1792004489479993697";
    auto idxIdentObj = BSON("_id_" << idxIdent);

    // Import should fail with missing "md" field.
    ASSERT_THROWS_CODE(
        importCollectionTest(
            tenantNs,
            BSON("idxIdent" << idxIdentObj << "ns" << tenantNs.toString() << "ident" << ident)),
        AssertionException,
        ErrorCodes::BadValue);

    // Import should fail with missing "ident" field.
    ASSERT_THROWS_CODE(importCollectionTest(tenantNs,
                                            BSON("md" << mdObj << "idxIdent" << idxIdentObj << "ns"
                                                      << tenantNs.toString())),
                       AssertionException,
                       ErrorCodes::BadValue);

    // Import should success with validate inputs.
    auto swImportResult =
        importCollectionTest(tenantNs,
                             BSON("md" << mdObj << "idxIdent" << idxIdentObj << "ns"
                                       << tenantNs.toString() << "ident" << ident));
    ASSERT_OK(swImportResult.getStatus());
    DurableCatalog::ImportResult importResult = std::move(swImportResult.getValue());

    // Validate the catalog entry for the imported collection.
    auto entry = getCatalog()->getEntry(importResult.catalogId);
    ASSERT_EQ(entry.tenantNs, tenantNs);
    ASSERT_EQ(entry.ident, ident);
    ASSERT_EQ(getCatalog()->getIndexIdent(operationContext(), importResult.catalogId, "_id_"),
              idxIdent);

    // Test that a collection UUID is generated for import.
    ASSERT_NE(optionsWithUUID.uuid.get(), importResult.uuid);
    // Substitute in the generated UUID and check that the rest of fields in the catalog entry
    // match.
    md.options.uuid = importResult.uuid;
    ASSERT_BSONOBJ_EQ(getCatalog()->getCatalogEntry(operationContext(), importResult.catalogId),
                      BSON("md" << md.toBSON() << "idxIdent" << idxIdentObj << "ns"
                                << tenantNs.toString() << "ident" << ident));
}

TEST_F(DurableCatalogTest, IdentSuffixUsesRand) {
    const std::string rand = "0000000000000000000";
    getCatalog()->setRand_forTest(rand);

    const TenantNamespace tenantNs = TenantNamespace(boost::none, NamespaceString("a.b"));

    auto uuid = createCollection(tenantNs, CollectionOptions());
    auto collection = CollectionCatalog::get(operationContext())
                          ->lookupCollectionByUUID(operationContext(), uuid);
    RecordId catalogId = collection->getCatalogId();
    ASSERT(StringData(getCatalog()->getEntry(catalogId).ident).endsWith(rand));
    ASSERT_EQUALS(getCatalog()->getRand_forTest(), rand);
}

TEST_F(DurableCatalogTest, ImportCollectionRandConflict) {
    const std::string rand = "0000000000000000000";
    getCatalog()->setRand_forTest(rand);

    {
        // Import a collection with the 'rand' suffix as part of the ident. This will force 'rand'
        // to be changed in the durable catalog internals.
        const TenantNamespace tenantNs =
            TenantNamespace(boost::none, NamespaceString("unittest.import"));
        BSONCollectionCatalogEntry::MetaData md;
        md.tenantNs = tenantNs;

        CollectionOptions optionsWithUUID;
        optionsWithUUID.uuid = UUID::gen();
        md.options = optionsWithUUID;

        BSONCollectionCatalogEntry::IndexMetaData indexMetaData;
        indexMetaData.spec = BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                      << "_id_");
        indexMetaData.ready = true;
        md.indexes.push_back(indexMetaData);

        auto mdObj = md.toBSON();
        const auto ident = "collection-0-" + rand;
        const auto idxIdent = "index-0-" + rand;
        auto idxIdentObj = BSON("_id_" << idxIdent);

        auto swImportResult =
            importCollectionTest(tenantNs,
                                 BSON("md" << mdObj << "idxIdent" << idxIdentObj << "ns"
                                           << tenantNs.toString() << "ident" << ident));
        ASSERT_OK(swImportResult.getStatus());
    }

    ASSERT_NOT_EQUALS(getCatalog()->getRand_forTest(), rand);

    {
        // Check that a newly created collection doesn't use 'rand' as the suffix in the ident.
        const TenantNamespace tenantNs = TenantNamespace(boost::none, NamespaceString("a.b"));
        createCollection(tenantNs, CollectionOptions());

        RecordId catalogId = getCollection()->getCatalogId();
        ASSERT(!StringData(getCatalog()->getEntry(catalogId).ident).endsWith(rand));
    }

    ASSERT_NOT_EQUALS(getCatalog()->getRand_forTest(), rand);
}

TEST_F(DurableCatalogTest, CheckTimeseriesBucketsMayHaveMixedSchemaDataFlagFCVLatest) {
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLatest);

    {
        const TenantNamespace regularTenantNs =
            TenantNamespace(boost::none, NamespaceString("test.regular"));
        createCollection(regularTenantNs, CollectionOptions());

        auto collection =
            CollectionCatalog::get(operationContext())
                ->lookupCollectionByNamespace(operationContext(), regularTenantNs.getNss());
        RecordId catalogId = collection->getCatalogId();
        ASSERT(!getCatalog()
                    ->getMetaData(operationContext(), catalogId)
                    ->timeseriesBucketsMayHaveMixedSchemaData);
    }

    {
        const TenantNamespace bucketsTenantNs =
            TenantNamespace(boost::none, NamespaceString("system.buckets.ts"));
        CollectionOptions options;
        options.timeseries = TimeseriesOptions(/*timeField=*/"t");
        createCollection(bucketsTenantNs, options);

        auto collection =
            CollectionCatalog::get(operationContext())
                ->lookupCollectionByNamespace(operationContext(), bucketsTenantNs.getNss());
        RecordId catalogId = collection->getCatalogId();
        ASSERT(getCatalog()
                   ->getMetaData(operationContext(), catalogId)
                   ->timeseriesBucketsMayHaveMixedSchemaData);
        ASSERT_FALSE(*getCatalog()
                          ->getMetaData(operationContext(), catalogId)
                          ->timeseriesBucketsMayHaveMixedSchemaData);
    }
}

TEST_F(DurableCatalogTest, CreateCollectionCatalogEntryHasCorrectTenantNamespace) {
    gMultitenancySupport = true;

    auto tenantId = TenantId(OID::gen());
    const TenantNamespace tenantNs = TenantNamespace(tenantId, NamespaceString("test.regular"));
    createCollection(tenantNs, CollectionOptions());

    auto collection = CollectionCatalog::get(operationContext())
                          ->lookupCollectionByNamespace(operationContext(), tenantNs.getNss());
    RecordId catalogId = collection->getCatalogId();
    ASSERT_EQ(getCatalog()->getEntry(catalogId).tenantNs, tenantNs);
}

TEST_F(DurableCatalogTest, ImportCollectionCatalogEntryHasCorrectTenantNamespace) {
    gMultitenancySupport = true;

    auto tenantId = TenantId(OID::gen());
    const TenantNamespace tenantNs = TenantNamespace(tenantId, NamespaceString("unittest.import"));

    BSONCollectionCatalogEntry::MetaData md;
    md.tenantNs = tenantNs;
    CollectionOptions optionsWithUUID;
    optionsWithUUID.uuid = UUID::gen();
    md.options = optionsWithUUID;
    auto mdObj = md.toBSON();
    const auto ident = "collection-1-1234567891234567899";

    auto swImportResult = importCollectionTest(
        tenantNs, BSON("md" << mdObj << "ns" << tenantNs.toString() << "ident" << ident));
    ASSERT_OK(swImportResult.getStatus());

    auto entry = getCatalog()->getEntry(swImportResult.getValue().catalogId);
    ASSERT_EQ(entry.tenantNs, tenantNs);
    ASSERT_EQ(entry.ident, ident);
}

}  // namespace
}  // namespace mongo
