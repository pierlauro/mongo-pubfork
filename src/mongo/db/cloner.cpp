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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/cloner.h"
#include "mongo/db/cloner_gen.h"

#include <algorithm>

#include "mongo/base/status.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

namespace mongo {

using IndexVersion = IndexDescriptor::IndexVersion;

MONGO_FAIL_POINT_DEFINE(movePrimaryFailPoint);

BSONElement getErrField(const BSONObj& o);

BSONObj Cloner::_getIdIndexSpec(const std::list<BSONObj>& indexSpecs) {
    for (auto&& indexSpec : indexSpecs) {
        BSONElement indexName;
        uassertStatusOK(bsonExtractTypedField(
            indexSpec, IndexDescriptor::kIndexNameFieldName, String, &indexName));
        if (indexName.valueStringData() == "_id_"_sd) {
            return indexSpec;
        }
    }
    return BSONObj();
}

Cloner::Cloner() {}

struct Cloner::Fun {
    Fun(OperationContext* opCtx, const std::string& dbName)
        : lastLog(0), opCtx(opCtx), _dbName(dbName) {}

    void operator()(DBClientCursorBatchIterator& i) {
        boost::optional<Lock::DBLock> dbLock;
        dbLock.emplace(opCtx, _dbName, MODE_X);
        uassert(ErrorCodes::NotMaster,
                str::stream() << "Not primary while cloning collection " << nss,
                !opCtx->writesAreReplicated() ||
                    repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

        // Make sure database still exists after we resume from the temp release
        auto databaseHolder = DatabaseHolder::get(opCtx);
        auto db = databaseHolder->openDb(opCtx, _dbName);
        auto collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
        if (!collection) {
            writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
                opCtx->checkForInterrupt();

                WriteUnitOfWork wunit(opCtx);
                const bool createDefaultIndexes = true;
                CollectionOptions collectionOptions = uassertStatusOK(CollectionOptions::parse(
                    from_options, CollectionOptions::ParseKind::parseForCommand));
                invariant(db->userCreateNS(
                              opCtx, nss, collectionOptions, createDefaultIndexes, from_id_index),
                          str::stream()
                              << "collection creation failed during clone [" << nss << "]");
                wunit.commit();
                collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
                invariant(collection,
                          str::stream() << "Missing collection during clone [" << nss << "]");
            });
        }

        while (i.moreInCurrentBatch()) {
            if (numSeen % 128 == 127) {
                time_t now = time(nullptr);
                if (now - lastLog >= 60) {
                    // report progress
                    if (lastLog)
                        LOGV2(20412, "clone", "ns"_attr = nss, "numSeen"_attr = numSeen);
                    lastLog = now;
                }
                opCtx->checkForInterrupt();

                dbLock.reset();

                CurOp::get(opCtx)->yielded();

                dbLock.emplace(opCtx, _dbName, MODE_X);

                // Check if everything is still all right.
                if (opCtx->writesAreReplicated()) {
                    uassert(
                        ErrorCodes::PrimarySteppedDown,
                        str::stream() << "Cannot write to ns: " << nss << " after yielding",
                        repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));
                }

                db = databaseHolder->getDb(opCtx, _dbName);
                uassert(28593,
                        str::stream() << "Database " << _dbName << " dropped while cloning",
                        db != nullptr);

                collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
                uassert(28594,
                        str::stream() << "Collection " << nss << " dropped while cloning",
                        collection != nullptr);
            }

            BSONObj tmp = i.nextSafe();

            /* assure object is valid.  note this will slow us down a little. */
            // Use the latest BSON validation version. We allow cloning of collections containing
            // decimal data even if decimal is disabled.
            const Status status = validateBSON(tmp.objdata(), tmp.objsize(), BSONVersion::kLatest);
            if (!status.isOK()) {
                str::stream ss;
                ss << "Cloner: found corrupt document in " << nss << ": " << redact(status);
                if (gSkipCorruptDocumentsWhenCloning.load()) {
                    LOGV2_WARNING(20423, "{ss_ss_str}; skipping", "ss_ss_str"_attr = ss.ss.str());
                    continue;
                }
                msgasserted(28531, ss);
            }

            verify(collection);
            ++numSeen;

            writeConflictRetry(opCtx, "cloner insert", nss.ns(), [&] {
                opCtx->checkForInterrupt();

                WriteUnitOfWork wunit(opCtx);

                BSONObj doc = tmp;
                OpDebug* const nullOpDebug = nullptr;
                Status status =
                    collection->insertDocument(opCtx, InsertStatement(doc), nullOpDebug, true);
                if (!status.isOK() && status.code() != ErrorCodes::DuplicateKey) {
                    LOGV2_ERROR(20424,
                                "error: exception cloning object",
                                "ns"_attr = nss,
                                "status"_attr = redact(status),
                                "doc"_attr = redact(doc));
                    uassertStatusOK(status);
                }
                if (status.isOK()) {
                    wunit.commit();
                }
            });

            static Rarely sampler;
            if (sampler.tick() && (time(nullptr) - saveLast > 60)) {
                LOGV2(20413,
                      "objects cloned so far from collection",
                      "numSeen"_attr = numSeen,
                      "ns"_attr = nss);
                saveLast = time(nullptr);
            }
        }
    }

    time_t lastLog;
    OperationContext* opCtx;
    const std::string _dbName;

    int64_t numSeen;
    NamespaceString nss;
    BSONObj from_options;
    BSONObj from_id_index;
    time_t saveLast;
};

/* copy the specified collection
 */
void Cloner::_copy(OperationContext* opCtx,
                   const std::string& toDBName,
                   const NamespaceString& nss,
                   const BSONObj& from_opts,
                   const BSONObj& from_id_index,
                   Query query,
                   DBClientBase* conn) {
    LOGV2_DEBUG(20414,
                2,
                "\t\tcloning collection with filter",
                "ns"_attr = nss,
                "conn_getServerAddress"_attr = conn->getServerAddress(),
                "query"_attr = redact(query.toString()));

    Fun f(opCtx, toDBName);
    f.numSeen = 0;
    f.nss = nss;
    f.from_options = from_opts;
    f.from_id_index = from_id_index;
    f.saveLast = time(nullptr);

    int options = QueryOption_NoCursorTimeout | QueryOption_Exhaust;
    {
        Lock::TempRelease tempRelease(opCtx->lockState());
        conn->query(std::function<void(DBClientCursorBatchIterator&)>(f),
                    nss,
                    query,
                    nullptr,
                    options,
                    0 /* batchSize */,
                    repl::ReadConcernArgs::kImplicitDefault);
    }

    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while cloning collection " << nss.ns() << " with filter "
                          << query.toString(),
            !opCtx->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));
}

void Cloner::_copyIndexes(OperationContext* opCtx,
                          const std::string& toDBName,
                          const NamespaceString& nss,
                          const BSONObj& from_opts,
                          const std::list<BSONObj>& from_indexes,
                          DBClientBase* conn) {
    LOGV2_DEBUG(20415,
                2,
                "\t\t copyIndexes",
                "ns"_attr = nss,
                "conn_getServerAddress"_attr = conn->getServerAddress());

    uassert(ErrorCodes::PrimarySteppedDown,
            str::stream() << "Not primary while copying indexes from " << nss << " (Cloner)",
            !opCtx->writesAreReplicated() ||
                repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

    if (from_indexes.empty())
        return;

    // We are under lock here again, so reload the database in case it may have disappeared
    // during the temp release
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->openDb(opCtx, toDBName);

    Collection* collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
    if (!collection) {
        writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
            opCtx->checkForInterrupt();

            WriteUnitOfWork wunit(opCtx);
            CollectionOptions collectionOptions = uassertStatusOK(
                CollectionOptions::parse(from_opts, CollectionOptions::ParseKind::parseForCommand));
            const bool createDefaultIndexes = true;
            invariant(db->userCreateNS(opCtx,
                                       nss,
                                       collectionOptions,
                                       createDefaultIndexes,
                                       _getIdIndexSpec(from_indexes)),
                      str::stream() << "Collection creation failed while copying indexes from "
                                    << nss << " (Cloner)");
            wunit.commit();
            collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
            invariant(collection, str::stream() << "Missing collection " << nss << " (Cloner)");
        });
    }

    auto indexCatalog = collection->getIndexCatalog();
    auto indexesToBuild = indexCatalog->removeExistingIndexesNoChecks(
        opCtx, {std::begin(from_indexes), std::end(from_indexes)});
    if (indexesToBuild.empty()) {
        return;
    }

    // TODO pass the MultiIndexBlock when inserting into the collection rather than building the
    // indexes after the fact. This depends on holding a lock on the collection the whole time
    // from creation to completion without yielding to ensure the index and the collection
    // matches. It also wouldn't work on non-empty collections so we would need both
    // implementations anyway as long as that is supported.
    MultiIndexBlock indexer;

    // Emit startIndexBuild and commitIndexBuild oplog entries if supported by the current FCV.
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto fromMigrate = false;
    auto buildUUID = IndexBuildsCoordinator::supportsTwoPhaseIndexBuild()
        ? boost::make_optional(UUID::gen())
        : boost::none;

    MultiIndexBlock::OnInitFn onInitFn;
    if (opCtx->writesAreReplicated() && buildUUID) {
        onInitFn = [&](std::vector<BSONObj>& specs) {
            // TODO SERVER-47438: Should remove this onInitFn lambda function as we no longer
            // need to generate startIndexBuild and commitIndexBuild oplog entries.

            // Currently, primary doesn't wait for any votes from secondaries to commit
            // the index build. So, it's of no use to set the commit quorum option of any value
            // greater than 0. Disabling commit quorum is just an optimization to avoid secondaries
            // from trying to vote before committing index build.
            //
            // Persist the commit quorum value in the config.system.indexBuilds collection.
            IndexBuildEntry indexbuildEntry(*buildUUID,
                                            collection->uuid(),
                                            CommitQuorumOptions(CommitQuorumOptions::kDisabled),
                                            IndexBuildsCoordinator::extractIndexNames(specs));
            uassertStatusOK(indexbuildentryhelpers::addIndexBuildEntry(opCtx, indexbuildEntry));

            opObserver->onStartIndexBuild(
                opCtx, nss, collection->uuid(), *buildUUID, specs, fromMigrate);
            return Status::OK();
        };
    } else {
        onInitFn = MultiIndexBlock::kNoopOnInitFn;
    }

    auto indexInfoObjs = uassertStatusOK(indexer.init(opCtx, collection, indexesToBuild, onInitFn));

    // The code below throws, so ensure build cleanup occurs.
    auto abortOnExit = makeGuard(
        [&] { indexer.abortIndexBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn); });

    uassertStatusOK(indexer.insertAllDocumentsInCollection(opCtx, collection));
    uassertStatusOK(indexer.checkConstraints(opCtx));

    WriteUnitOfWork wunit(opCtx);
    uassertStatusOK(
        indexer.commit(opCtx,
                       collection,
                       [&](const BSONObj& spec) {
                           // If two phase index builds are enabled, the index build will be
                           // coordinated using startIndexBuild and commitIndexBuild oplog entries.
                           if (opCtx->writesAreReplicated() &&
                               !IndexBuildsCoordinator::supportsTwoPhaseIndexBuild()) {
                               opObserver->onCreateIndex(
                                   opCtx, collection->ns(), collection->uuid(), spec, fromMigrate);
                           }
                       },
                       [&] {
                           if (opCtx->writesAreReplicated() && buildUUID) {
                               opObserver->onCommitIndexBuild(opCtx,
                                                              collection->ns(),
                                                              collection->uuid(),
                                                              *buildUUID,
                                                              indexInfoObjs,
                                                              fromMigrate);
                           }
                       }));
    wunit.commit();
    abortOnExit.dismiss();
}

StatusWith<std::vector<BSONObj>> Cloner::_filterCollectionsForClone(
    const std::string& fromDBName, const std::list<BSONObj>& initialCollections) {
    std::vector<BSONObj> finalCollections;
    for (auto&& collection : initialCollections) {
        LOGV2_DEBUG(20418, 2, "\t cloner got {collection}", "collection"_attr = collection);

        BSONElement collectionOptions = collection["options"];
        if (collectionOptions.isABSONObj()) {
            auto statusWithCollectionOptions = CollectionOptions::parse(
                collectionOptions.Obj(), CollectionOptions::ParseKind::parseForCommand);
            if (!statusWithCollectionOptions.isOK()) {
                return statusWithCollectionOptions.getStatus();
            }
        }

        std::string collectionName;
        auto status = bsonExtractStringField(collection, "name", &collectionName);
        if (!status.isOK()) {
            return status;
        }

        const NamespaceString ns(fromDBName, collectionName.c_str());

        if (ns.isSystem()) {
            if (!ns.isLegalClientSystemNS()) {
                LOGV2_DEBUG(20419, 2, "\t\t not cloning because system collection");
                continue;
            }
        }

        finalCollections.push_back(collection.getOwned());
    }
    return finalCollections;
}

Status Cloner::_createCollectionsForDb(
    OperationContext* opCtx,
    const std::vector<CreateCollectionParams>& createCollectionParams,
    const std::string& dbName) {
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->openDb(opCtx, dbName);
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));

    auto collCount = 0;
    for (auto&& params : createCollectionParams) {
        if (MONGO_unlikely(movePrimaryFailPoint.shouldFail()) && collCount > 0) {
            return Status(ErrorCodes::CommandFailed, "movePrimary failed due to failpoint");
        }
        collCount++;

        BSONObjBuilder optionsBuilder;
        optionsBuilder.appendElements(params.collectionInfo["options"].Obj());

        const NamespaceString nss(dbName, params.collectionName);

        uassertStatusOK(userAllowedCreateNS(dbName, params.collectionName));
        Status status = writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
            opCtx->checkForInterrupt();
            WriteUnitOfWork wunit(opCtx);

            Collection* collection =
                CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
            if (collection) {
                if (!params.shardedColl) {
                    // If the collection is unsharded then we want to fail when a collection
                    // we're trying to create already exists.
                    return Status(ErrorCodes::NamespaceExists,
                                  str::stream() << "unsharded collection with same namespace "
                                                << nss.ns() << " already exists.");
                }

                // If the collection is sharded and a collection with the same name already
                // exists on the target, we check if the existing collection's UUID matches
                // that of the one we're trying to create. If it does, we treat the create
                // as a no-op; if it doesn't match, we return an error.
                auto existingOpts = DurableCatalog::get(opCtx)->getCollectionOptions(
                    opCtx, collection->getCatalogId());
                const UUID clonedUUID =
                    uassertStatusOK(UUID::parse(params.collectionInfo["info"]["uuid"]));

                if (clonedUUID == existingOpts.uuid)
                    return Status::OK();

                return Status(ErrorCodes::InvalidOptions,
                              str::stream()
                                  << "sharded collection with same namespace " << nss.ns()
                                  << " already exists, but UUIDs don't match. Existing UUID is "
                                  << existingOpts.uuid << " and new UUID is " << clonedUUID);
            }

            // If the collection does not already exist and is sharded, we create a new
            // collection on the target shard with the UUID of the original collection and
            // copy the options and secondary indexes. If the collection does not already
            // exist and is unsharded, we create a new collection with its own UUID and
            // copy the options and secondary indexes of the original collection.

            if (params.shardedColl) {
                optionsBuilder.append(params.collectionInfo["info"]["uuid"]);
            }

            const bool createDefaultIndexes = true;
            auto options = optionsBuilder.obj();

            CollectionOptions collectionOptions = uassertStatusOK(
                CollectionOptions::parse(options, CollectionOptions::ParseKind::parseForStorage));
            Status createStatus = db->userCreateNS(
                opCtx, nss, collectionOptions, createDefaultIndexes, params.idIndexSpec);
            if (!createStatus.isOK()) {
                return createStatus;
            }

            wunit.commit();
            return Status::OK();
        });

        // Break early if one of the creations fails.
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status Cloner::copyDb(OperationContext* opCtx,
                      const std::string& dBName,
                      const std::string& masterHost,
                      const std::vector<NamespaceString>& shardedColls,
                      std::set<std::string>* clonedColls) {
    invariant(clonedColls, str::stream() << masterHost << ":" << dBName);

    auto statusWithMasterHost = ConnectionString::parse(masterHost);
    if (!statusWithMasterHost.isOK()) {
        return statusWithMasterHost.getStatus();
    }

    const ConnectionString cs(statusWithMasterHost.getValue());

    bool masterSameProcess = false;
    std::vector<HostAndPort> csServers = cs.getServers();
    for (std::vector<HostAndPort>::const_iterator iter = csServers.begin(); iter != csServers.end();
         ++iter) {
        if (!repl::isSelf(*iter, opCtx->getServiceContext()))
            continue;

        masterSameProcess = true;
        break;
    }

    if (masterSameProcess) {
        // Guard against re-entrance
        return Status(ErrorCodes::IllegalOperation, "can't clone from self (localhost)");
    }

    // Set up connection.
    std::string errmsg;
    std::unique_ptr<DBClientBase> conn(cs.connect(StringData(), errmsg));
    if (!conn.get()) {
        return Status(ErrorCodes::HostUnreachable, errmsg);
    }

    if (auth::isInternalAuthSet()) {
        auto authStatus = conn->authenticateInternalUser();
        if (!authStatus.isOK()) {
            return authStatus;
        }
    }

    // Gather the list of collections to clone
    std::vector<BSONObj> toClone;
    clonedColls->clear();

    {
        // getCollectionInfos may make a remote call, which may block indefinitely, so release
        // the global lock that we are entering with.
        Lock::TempRelease tempRelease(opCtx->lockState());

        std::list<BSONObj> initialCollections =
            conn->getCollectionInfos(dBName, ListCollectionsFilter::makeTypeCollectionFilter());

        auto status = _filterCollectionsForClone(dBName, initialCollections);
        if (!status.isOK()) {
            return status.getStatus();
        }
        toClone = status.getValue();
    }

    std::vector<CreateCollectionParams> createCollectionParams;
    for (auto&& collection : toClone) {
        CreateCollectionParams params;
        params.collectionName = collection["name"].String();
        params.collectionInfo = collection;
        if (auto idIndex = collection["idIndex"]) {
            params.idIndexSpec = idIndex.Obj();
        }

        const NamespaceString ns(dBName, params.collectionName);
        if (std::find(shardedColls.begin(), shardedColls.end(), ns) != shardedColls.end()) {
            params.shardedColl = true;
        }
        createCollectionParams.push_back(params);
    }

    // Get index specs for each collection.
    std::map<StringData, std::list<BSONObj>> collectionIndexSpecs;
    {
        Lock::TempRelease tempRelease(opCtx->lockState());
        for (auto&& params : createCollectionParams) {
            const NamespaceString nss(dBName, params.collectionName);
            auto indexSpecs = conn->getIndexSpecs(nss);

            collectionIndexSpecs[params.collectionName] = indexSpecs;

            if (params.idIndexSpec.isEmpty()) {
                params.idIndexSpec = _getIdIndexSpec(indexSpecs);
            }
        }
    }

    uassert(
        ErrorCodes::NotMaster,
        str::stream() << "Not primary while cloning database " << dBName
                      << " (after getting list of collections to clone)",
        !opCtx->writesAreReplicated() ||
            repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx, dBName));

    auto status = _createCollectionsForDb(opCtx, createCollectionParams, dBName);
    if (!status.isOK()) {
        return status;
    }

    for (auto&& params : createCollectionParams) {
        if (params.shardedColl) {
            continue;
        }

        LOGV2_DEBUG(20420,
                    2,
                    "  really will clone: {params_collectionInfo}",
                    "params_collectionInfo"_attr = params.collectionInfo);

        const NamespaceString nss(dBName, params.collectionName);

        clonedColls->insert(nss.ns());

        LOGV2_DEBUG(20421, 1, "\t\t cloning", "ns"_attr = nss, "host"_attr = masterHost);

        _copy(opCtx,
              dBName,
              nss,
              params.collectionInfo["options"].Obj(),
              params.idIndexSpec,
              Query(),
              conn.get());
    }

    // now build the secondary indexes
    for (auto&& params : createCollectionParams) {
        LOGV2(20422,
              "copying indexes for: {params_collectionInfo}",
              "params_collectionInfo"_attr = params.collectionInfo);

        const NamespaceString nss(dBName, params.collectionName);


        _copyIndexes(opCtx,
                     dBName,
                     nss,
                     params.collectionInfo["options"].Obj(),
                     collectionIndexSpecs[params.collectionName],
                     conn.get());
    }

    return Status::OK();
}

}  // namespace mongo
