'use strict';

/**
 * collection_defragmentation.js
 *
 * Runs defragmentation on collections with concurrent operations.
 *
 * @tags: [requires_sharding, assumes_balancer_on]
 */

const dbPrefix = 'fsmDB_';
const dbCount = 2;
const collPrefix = 'sharded_coll_';
const collCount = 2;
const maxChunkSizeMB = 10;

load('jstests/sharding/libs/defragmentation_util.js');
load('jstests/sharding/libs/find_chunks_util.js');
load('jstests/concurrency/fsm_workload_helpers/chunks.js');

function getRandomDb(db) {
    return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
}

function getRandomCollection(db) {
    return db[collPrefix + Random.randInt(collCount)];
}

function getCollectionShardKey(configDB, ns) {
    const collection = configDB.collections.findOne({_id: ns});
    return collection.key;
}

function getExtendedCollectionShardKey(configDB, ns) {
    const currentShardKey = getCollectionShardKey(configDB, ns);
    const newCount = Object.keys(currentShardKey).length;
    currentShardKey["key" + newCount] = 1;
    return currentShardKey;
}

var $config = (function() {
    var states = {
        init: function init(db, collName, connCache) {
            // Initialize defragmentation
            for (let i = 0; i < dbCount; i++) {
                const dbName = dbPrefix + i;
                for (let j = 0; j < collCount; j++) {
                    const fullNs = dbName + "." + collPrefix + j;
                    assertWhenOwnColl.commandWorked(connCache.mongos[0].adminCommand({
                        configureCollectionBalancing: fullNs,
                        defragmentCollection: true,
                        chunkSize: maxChunkSizeMB,
                    }));
                }
            }
        },

        moveChunk: function moveChunk(db, collName, connCache) {
            const randomDB = getRandomDb(db);
            const randomColl = getRandomCollection(randomDB);
            const configDB = randomDB.getSiblingDB('config');
            const chunksJoinClause =
                findChunksUtil.getChunksJoinClause(configDB, randomColl.getFullName());
            const randomChunk =
                configDB.chunks.aggregate([{$match: chunksJoinClause}, {$sample: {size: 1}}])
                    .next();
            const fromShard = randomChunk.shard;
            const bounds = [randomChunk.min, randomChunk.max];
            const zoneForChunk = defragmentationUtil.getZoneForRange(
                connCache.mongos[0], randomColl.getFullName(), randomChunk.min, randomChunk.max);

            // Pick a shard at random to move it to. If the chunk is in a zone, look for a shard
            // with that zone.
            let shardFilter = {_id: {$ne: fromShard}};
            if (zoneForChunk !== null) {
                shardFilter['tag'] = zoneForChunk;
            }
            const shardCursor =
                configDB.shards.aggregate([{$match: shardFilter}, {$sample: {size: 1}}]);
            if (!shardCursor.hasNext()) {
                return;
            }
            const toShard = shardCursor.next();

            // Issue a moveChunk command.
            try {
                ChunkHelper.moveChunk(randomDB, randomColl.getName(), bounds, toShard['_id'], true);
                jsTest.log("Manual move chunk of chunk " + tojson(randomChunk) + " to shard " +
                           toShard['_id']);
            } catch (e) {
                jsTest.log("Ignoring manual move chunk error: " + tojson(e));
            }
        },

        mergeChunks: function mergeChunks(db, collName, connCache) {
            const randomDB = getRandomDb(db);
            const randomColl = getRandomCollection(randomDB);
            const configDB = randomDB.getSiblingDB('config');
            const keyPattern = getCollectionShardKey(configDB, randomColl.getFullName());

            // Get all the chunks
            const chunks = findChunksUtil.findChunksByNs(configDB, randomColl.getFullName())
                               .sort(keyPattern)
                               .toArray();

            // No chunks to merge is there are less than 2 chunks
            if (chunks.length < 2) {
                return;
            }

            // Choose a random starting point to look for mergeable chunks to make it less likely
            // that each thread tries to move the same chunk.
            let index = Random.randInt(chunks.length);
            for (let i = 0; i < chunks.length; i++) {
                if (index === chunks.length - 1) {
                    index = 0;
                }
                if (chunks[index].shard === chunks[index + 1].shard &&
                    defragmentationUtil.getZoneForRange(connCache.mongos[0],
                                                        randomColl.getFullName(),
                                                        chunks[index].min,
                                                        chunks[index].max) ===
                        defragmentationUtil.getZoneForRange(connCache.mongos[0],
                                                            randomColl.getFullName(),
                                                            chunks[index + 1].min,
                                                            chunks[index + 1].max)) {
                    const bounds = [chunks[index].min, chunks[index + 1].max];
                    try {
                        ChunkHelper.mergeChunks(randomDB, randomColl.getName(), bounds);
                        jsTest.log("Manual merge chunks of chunks " + tojson(chunks[index]) +
                                   " and " + tojson(chunks[index + 1]));
                    } catch (e) {
                        jsTest.log("Ignoring manual merge chunks error: " + tojson(e));
                    }
                    return;
                }
            }
        },

        splitChunks: function splitChunks(db, collName, connCache) {
            const randomDB = getRandomDb(db);
            const randomColl = getRandomCollection(randomDB);
            const configDB = randomDB.getSiblingDB('config');
            const chunksJoinClause =
                findChunksUtil.getChunksJoinClause(configDB, randomColl.getFullName());
            const randomChunk =
                configDB.chunks.aggregate([{$match: chunksJoinClause}, {$sample: {size: 1}}])
                    .toArray()[0];
            try {
                const res = connCache.shards[randomChunk.shard][0].getDB("admin").runCommand({
                    splitVector: randomColl.getFullName(),
                    keyPattern: getCollectionShardKey(configDB, randomColl.getFullName()),
                    min: randomChunk.min,
                    max: randomChunk.max,
                    force: true
                });
                assertAlways.commandWorked(res);
                ChunkHelper.splitChunkAt(randomDB, randomColl.getName(), res.splitKeys[0]);
                jsTest.log("Manual split chunk of chunk " + tojson(randomChunk) + " at " +
                           tojson(res.splitKeys[0]));
            } catch (e) {
                jsTest.log("Ignoring manual split chunk error: " + tojson(e));
            }
        },

        refineShardKey: function refineShardKey(db, collName, connCache) {
            const randomDB = getRandomDb(db);
            const randomColl = getRandomCollection(randomDB);
            const configDB = randomDB.getSiblingDB('config');
            const extendedShardKey =
                getExtendedCollectionShardKey(configDB, randomColl.getFullName());
            try {
                assertWhenOwnColl.commandWorked(randomColl.createIndex(extendedShardKey));
                assertWhenOwnColl.commandWorked(randomDB.adminCommand(
                    {refineCollectionShardKey: randomColl.getFullName(), key: extendedShardKey}));
                jsTest.log("Manual refine shard key for collection " + randomColl.getFullName() +
                           " to " + tojson(extendedShardKey));
            } catch (e) {
                jsTest.log("Ignoring manual refine shard key error: " + tojson(e));
            }
        }
    };

    var transitions = {
        init: {moveChunk: 0.25, mergeChunks: 0.25, splitChunks: 0.25, refineShardKey: 0.25},
        moveChunk: {mergeChunks: 0.33, splitChunks: 0.33, refineShardKey: 0.33},
        mergeChunks: {moveChunk: 0.33, splitChunks: 0.33, refineShardKey: 0.33},
        splitChunks: {moveChunk: 0.33, mergeChunks: 0.33, refineShardKey: 0.33},
        refineShardKey: {moveChunk: 0.33, mergeChunks: 0.33, splitChunks: 0.33},
    };

    function setup(db, collName, cluster) {
        const mongos = cluster.getDB('config').getMongo();
        // Create all fragmented collections
        for (let i = 0; i < dbCount; i++) {
            const dbName = dbPrefix + i;
            const newDb = db.getSiblingDB(dbName);
            assertAlways.commandWorked(newDb.adminCommand({enablesharding: dbName}));
            for (let j = 0; j < collCount; j++) {
                const fullNs = dbName + "." + collPrefix + j;
                const numChunks = Random.randInt(30);
                const numZones = Random.randInt(numChunks / 2);
                const docSizeBytes = Random.randInt(1024 * 1024) + 50;
                defragmentationUtil.createFragmentedCollection(
                    mongos,
                    fullNs,
                    numChunks,
                    5 /* maxChunkFillMB */,
                    numZones,
                    docSizeBytes,
                    1000 /* chunkSpacing */,
                    true /* disableCollectionBalancing*/);
            }
        }
    }

    function teardown(db, collName, cluster) {
        const mongos = cluster.getDB('config').getMongo();
        for (let i = 0; i < dbCount; i++) {
            const dbName = dbPrefix + i;
            for (let j = 0; j < collCount; j++) {
                const fullNs = dbName + "." + collPrefix + j;
                defragmentationUtil.waitForEndOfDefragmentation(mongos, fullNs);
                defragmentationUtil.checkPostDefragmentationState(
                    mongos, fullNs, maxChunkSizeMB, "key");
            }
        }
    }

    return {
        threadCount: 5,
        iterations: 10,
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
