/**
 * Test the shard merge rollback-to-stable algorithm.
 *
 * @tags: [
 *   does_not_support_encrypted_storage_engine,
 *   featureFlagShardMerge,
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_fcv_52,
 *   requires_journaling,
 *   requires_replication,
 *   requires_persistence,
 *   requires_wiredtiger,
 *   serverless,
 * ]
 */
(function() {
"use strict";

// TODO (SERVER-61144): Recipient secondaries don't import donor files yet, so dbhash will mismatch.
TestData.skipCheckDBHashes = true;

load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const migrationId = UUID();
const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

if (!TenantMigrationUtil.isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    return;
}

const kDataDir =
    `${recipientPrimary.dbpath}/migrationTmpFiles.${extractUUIDFromObject(migrationId)}`;
assert.eq(runNonMongoProgram("mkdir", "-p", kDataDir), 0);

(function() {
jsTestLog("Generate test data");

const db = donorPrimary.getDB("myDatabase");
const collection = db["myCollection"];
const capped = db["myCappedCollection"];
assert.commandWorked(db.createCollection("myCappedCollection", {capped: true, size: 100}));
for (let c of [collection, capped]) {
    c.insertMany([{_id: 0}, {_id: 1}, {_id: 2}], {writeConcern: {w: "majority"}});
}

assert.commandWorked(db.runCommand({
    createIndexes: "myCollection",
    indexes: [{key: {a: 1}, name: "a_1"}],
    writeConcern: {w: "majority"}
}));

// Ensure our new collections appear in the backup cursor's checkpoint.
assert.commandWorked(db.adminCommand({fsync: 1}));
})();

jsTestLog("Run migration");
// The old multitenant migrations won't copy myDatabase since it doesn't start with testTenantId,
// but shard merge copies everything so we still expect myDatabase on the recipient, below.
const kTenantId = "testTenantId";
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

// TODO SERVER-61144: Check on all recipient nodes that the collection documents got imported
// successfully.
for (let collectionName of ["myCollection", "myCappedCollection"]) {
    jsTestLog(`Checking ${collectionName}`);
    // Use "countDocuments" to check actual docs, "count" to check sizeStorer data.
    assert.eq(donorPrimary.getDB("myDatabase")[collectionName].countDocuments({}),
              recipientPrimary.getDB("myDatabase")[collectionName].countDocuments({}),
              "countDocuments");
    assert.eq(donorPrimary.getDB("myDatabase")[collectionName].count(),
              recipientPrimary.getDB("myDatabase")[collectionName].count(),
              "count");
}

tenantMigrationTest.stop();
})();
