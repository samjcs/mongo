/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include <boost/filesystem.hpp>
#include <string>

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Represents the file blocks returned by the storage engine during both full and incremental
 * backups. In the case of a full backup, each block is an entire file with offset=0 and
 * length=fileSize. In the case of the first basis for future incremental backups, each block is
 * an entire file with offset=0 and length=0. In the case of a subsequent incremental backup,
 * each block reflects changes made to data files since the basis (named 'thisBackupName') and
 * each block has a maximum size of 'blockSizeMB'.
 *
 * If a file is unchanged in a subsequent incremental backup, a single block is returned with
 * offset=0 and length=0. This allows consumers of the backup API to safely truncate files that
 * are not returned by the backup cursor.
 */
class BackupBlock final {
public:
    explicit BackupBlock(OperationContext* opCtx,
                         std::string filePath,
                         std::uint64_t offset = 0,
                         std::uint64_t length = 0,
                         std::uint64_t fileSize = 0);

    ~BackupBlock() = default;

    std::string filePath() const {
        return _filePath;
    }

    std::string ns() const {
        // Remove "system.buckets." from time-series collection namespaces since it is an
        // internal detail that is not intended to be visible externally.
        if (_nss.isTimeseriesBucketsCollection()) {
            return _nss.getTimeseriesViewNamespace().toString();
        }

        return _nss.toString();
    }

    std::uint64_t offset() const {
        return _offset;
    }

    std::uint64_t length() const {
        return _length;
    }

    std::uint64_t fileSize() const {
        return _fileSize;
    }

    boost::optional<UUID> uuid() const {
        return _uuid;
    }

    /**
     * Returns whether the file must be copied regardless of choice for selective backups.
     */
    bool isRequired() const;

private:
    /**
     * Sets '_nss' and '_uuid' for:
     * - collections
     * - indexes, to the NSS/UUID of their respective collection
     * A null opCtx is ignored. A null opCtx is exercised by FCBIS unit tests.
     */
    void _initialize(OperationContext* opCtx);
    void _setNamespaceString(OperationContext* opCtx, NamespaceString nss) {
        _nss = nss;
    }
    void _setUuid(OperationContext* opCtx, DurableCatalog* catalog, RecordId catalogId);

    const std::string _filePath;
    const std::uint64_t _offset;
    const std::uint64_t _length;
    const std::uint64_t _fileSize;

    std::string _filenameStem;
    NamespaceString _nss;
    boost::optional<UUID> _uuid;
};
}  // namespace mongo
