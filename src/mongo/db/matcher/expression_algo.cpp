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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/logv2/log.h"

namespace mongo {

using std::unique_ptr;

namespace {

bool supportsEquality(const ComparisonMatchExpression* expr) {
    switch (expr->matchType()) {
        case MatchExpression::LTE:
        case MatchExpression::EQ:
        case MatchExpression::GTE:
            return true;
        default:
            return false;
    }
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const ComparisonMatchExpression* lhs, const ComparisonMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    const BSONElement lhsData = lhs->getData();
    const BSONElement rhsData = rhs->getData();

    if (lhsData.canonicalType() != rhsData.canonicalType()) {
        return false;
    }

    // Special case the handling for NaN values: NaN compares equal only to itself.
    if (std::isnan(lhsData.numberDouble()) || std::isnan(rhsData.numberDouble())) {
        if (supportsEquality(lhs) && supportsEquality(rhs)) {
            return std::isnan(lhsData.numberDouble()) && std::isnan(rhsData.numberDouble());
        }
        return false;
    }

    if (!CollatorInterface::collatorsMatch(lhs->getCollator(), rhs->getCollator()) &&
        CollationIndexKey::isCollatableType(lhsData.type())) {
        return false;
    }

    // Either collator may be used by compareElements() here, since either the collators are
    // the same or lhsData does not contain string comparison.
    int cmp = BSONElement::compareElements(
        lhsData, rhsData, BSONElement::ComparisonRules::kConsiderFieldName, rhs->getCollator());

    // Check whether the two expressions are equivalent.
    if (lhs->matchType() == rhs->matchType() && cmp == 0) {
        return true;
    }

    switch (rhs->matchType()) {
        case MatchExpression::LT:
        case MatchExpression::LTE:
            switch (lhs->matchType()) {
                case MatchExpression::LT:
                case MatchExpression::LTE:
                case MatchExpression::EQ:
                    if (rhs->matchType() == MatchExpression::LTE) {
                        return cmp <= 0;
                    }
                    return cmp < 0;
                default:
                    return false;
            }
        case MatchExpression::GT:
        case MatchExpression::GTE:
            switch (lhs->matchType()) {
                case MatchExpression::GT:
                case MatchExpression::GTE:
                case MatchExpression::EQ:
                    if (rhs->matchType() == MatchExpression::GTE) {
                        return cmp >= 0;
                    }
                    return cmp > 0;
                default:
                    return false;
            }
        default:
            return false;
    }
}

bool _isSubsetOfInternalExpr(const ComparisonMatchExpressionBase* lhs,
                             const ComparisonMatchExpressionBase* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    const BSONElement lhsData = lhs->getData();
    const BSONElement rhsData = rhs->getData();

    if (!CollatorInterface::collatorsMatch(lhs->getCollator(), rhs->getCollator()) &&
        CollationIndexKey::isCollatableType(lhsData.type())) {
        return false;
    }

    int cmp = lhsData.woCompare(
        rhsData, BSONElement::ComparisonRules::kConsiderFieldName, rhs->getCollator());

    // Check whether the two expressions are equivalent.
    if (lhs->matchType() == rhs->matchType() && cmp == 0) {
        return true;
    }

    switch (rhs->matchType()) {
        case MatchExpression::INTERNAL_EXPR_LT:
        case MatchExpression::INTERNAL_EXPR_LTE:
            switch (lhs->matchType()) {
                case MatchExpression::INTERNAL_EXPR_LT:
                case MatchExpression::INTERNAL_EXPR_LTE:
                case MatchExpression::INTERNAL_EXPR_EQ:
                    //
                    if (rhs->matchType() == MatchExpression::LTE) {
                        return cmp <= 0;
                    }
                    return cmp < 0;
                default:
                    return false;
            }
        case MatchExpression::INTERNAL_EXPR_GT:
        case MatchExpression::INTERNAL_EXPR_GTE:
            switch (lhs->matchType()) {
                case MatchExpression::INTERNAL_EXPR_GT:
                case MatchExpression::INTERNAL_EXPR_GTE:
                case MatchExpression::INTERNAL_EXPR_EQ:
                    if (rhs->matchType() == MatchExpression::GTE) {
                        return cmp >= 0;
                    }
                    return cmp > 0;
                default:
                    return false;
            }
        default:
            return false;
    }
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 *
 * This overload handles the $_internalExpr family of comparisons.
 */
bool _isSubsetOfInternalExpr(const MatchExpression* lhs, const ComparisonMatchExpressionBase* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    if (ComparisonMatchExpressionBase::isInternalExprComparison(lhs->matchType())) {
        return _isSubsetOfInternalExpr(static_cast<const ComparisonMatchExpressionBase*>(lhs), rhs);
    }

    return false;
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 *
 * This overload handles comparisons such as $lt, $eq, $gte, but not $_internalExprLt, etc.
 */
bool _isSubsetOf(const MatchExpression* lhs, const ComparisonMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    if (ComparisonMatchExpression::isComparisonMatchExpression(lhs)) {
        return _isSubsetOf(static_cast<const ComparisonMatchExpression*>(lhs), rhs);
    }

    if (lhs->matchType() == MatchExpression::MATCH_IN) {
        const InMatchExpression* ime = static_cast<const InMatchExpression*>(lhs);
        if (!ime->getRegexes().empty()) {
            return false;
        }
        for (BSONElement elem : ime->getEqualities()) {
            // Each element in the $in-array represents an equality predicate.
            EqualityMatchExpression equality(lhs->path(), elem);
            equality.setCollator(ime->getCollator());
            if (!_isSubsetOf(&equality, rhs)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const MatchExpression* lhs, const InMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field.
    if (lhs->path() != rhs->path()) {
        return false;
    }

    if (!rhs->getRegexes().empty()) {
        return false;
    }

    for (BSONElement elem : rhs->getEqualities()) {
        // Each element in the $in-array represents an equality predicate.
        EqualityMatchExpression equality(rhs->path(), elem);
        equality.setCollator(rhs->getCollator());
        if (_isSubsetOf(lhs, &equality)) {
            return true;
        }
    }
    return false;
}

/**
 * Returns true if the documents matched by 'lhs' are a subset of the documents matched by
 * 'rhs', i.e. a document matched by 'lhs' must also be matched by 'rhs', and false otherwise.
 */
bool _isSubsetOf(const MatchExpression* lhs, const ExistsMatchExpression* rhs) {
    // An expression can only match a subset of the documents matched by another if they are
    // comparing the same field. Defer checking the path for $not expressions until the
    // subexpression is examined.
    if (lhs->matchType() != MatchExpression::NOT && lhs->path() != rhs->path()) {
        return false;
    }

    if (ComparisonMatchExpression::isComparisonMatchExpression(lhs)) {
        const ComparisonMatchExpression* cme = static_cast<const ComparisonMatchExpression*>(lhs);
        // The CompareMatchExpression constructor prohibits creating a match expression with EOO or
        // Undefined types, so only need to ensure that the value is not of type jstNULL.
        return cme->getData().type() != jstNULL;
    }

    switch (lhs->matchType()) {
        case MatchExpression::ELEM_MATCH_VALUE:
        case MatchExpression::ELEM_MATCH_OBJECT:
        case MatchExpression::EXISTS:
        case MatchExpression::GEO:
        case MatchExpression::MOD:
        case MatchExpression::REGEX:
        case MatchExpression::SIZE:
        case MatchExpression::TYPE_OPERATOR:
            return true;
        case MatchExpression::MATCH_IN: {
            const InMatchExpression* ime = static_cast<const InMatchExpression*>(lhs);
            return !ime->hasNull();
        }
        case MatchExpression::NOT:
            // An expression can only match a subset of the documents matched by another if they are
            // comparing the same field.
            if (lhs->getChild(0)->path() != rhs->path()) {
                return false;
            }

            switch (lhs->getChild(0)->matchType()) {
                case MatchExpression::EQ: {
                    const ComparisonMatchExpression* cme =
                        static_cast<const ComparisonMatchExpression*>(lhs->getChild(0));
                    return cme->getData().type() == jstNULL;
                }
                case MatchExpression::MATCH_IN: {
                    const InMatchExpression* ime =
                        static_cast<const InMatchExpression*>(lhs->getChild(0));
                    return ime->hasNull();
                }
                default:
                    return false;
            }
        default:
            return false;
    }
}

/**
 * Creates a MatchExpression that is equivalent to {$and: [children[0], children[1]...]}.
 */
unique_ptr<MatchExpression> createAndOfNodes(std::vector<unique_ptr<MatchExpression>>* children) {
    if (children->empty()) {
        return nullptr;
    }

    if (children->size() == 1) {
        return std::move(children->at(0));
    }

    unique_ptr<AndMatchExpression> splitAnd = std::make_unique<AndMatchExpression>();
    for (auto&& expr : *children)
        splitAnd->add(std::move(expr));

    return splitAnd;
}

/**
 * Creates a MatchExpression that is equivalent to {$nor: [children[0], children[1]...]}.
 */
unique_ptr<MatchExpression> createNorOfNodes(std::vector<unique_ptr<MatchExpression>>* children) {
    if (children->empty()) {
        return nullptr;
    }

    unique_ptr<NorMatchExpression> splitNor = std::make_unique<NorMatchExpression>();
    for (auto&& expr : *children)
        splitNor->add(std::move(expr));

    return splitNor;
}

/**
 * Attempt to split 'expr' into two MatchExpressions according to 'shouldSplitOut', which describes
 * the conditions under which its argument can be split from 'expr'. Returns two pointers, where
 * each new MatchExpression contains a portion of 'expr'. The first contains the parts of 'expr'
 * which satisfy 'shouldSplitOut', and the second are the remaining parts of 'expr'.
 */
std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitMatchExpressionByFunction(
    unique_ptr<MatchExpression> expr,
    const std::set<std::string>& fields,
    expression::ShouldSplitExprFunc shouldSplitOut) {
    if (shouldSplitOut(*expr, fields)) {
        // 'expr' satisfies our split condition and can be completely split out.
        return {std::move(expr), nullptr};
    }

    if (expr->getCategory() != MatchExpression::MatchCategory::kLogical) {
        // 'expr' is a leaf and cannot be split out.
        return {nullptr, std::move(expr)};
    }

    std::vector<unique_ptr<MatchExpression>> splitOut;
    std::vector<unique_ptr<MatchExpression>> remaining;

    switch (expr->matchType()) {
        case MatchExpression::AND: {
            auto andExpr = checked_cast<AndMatchExpression*>(expr.get());
            for (size_t i = 0; i < andExpr->numChildren(); i++) {
                auto children = splitMatchExpressionByFunction(
                    andExpr->releaseChild(i), fields, shouldSplitOut);

                invariant(children.first || children.second);

                if (children.first) {
                    splitOut.push_back(std::move(children.first));
                }
                if (children.second) {
                    remaining.push_back(std::move(children.second));
                }
            }
            return {createAndOfNodes(&splitOut), createAndOfNodes(&remaining)};
        }
        case MatchExpression::NOR: {
            // We can split a $nor because !(x | y) is logically equivalent to !x & !y.

            // However, we cannot split each child individually; instead, we must look for a wholly
            // independent child to split off by itself. As an example of why, with 'b' in
            // 'fields': $nor: [{$and: [{a: 1}, {b: 1}]}]} will match if a is not 1, or if b is not
            // 1. However, if we split this into: {$nor: [{$and: [{a: 1}]}]}, and
            // {$nor: [{$and: [{b: 1}]}]}, a document will only pass both stages if neither a nor b
            // is equal to 1.
            auto norExpr = checked_cast<NorMatchExpression*>(expr.get());
            for (size_t i = 0; i < norExpr->numChildren(); i++) {
                auto child = norExpr->releaseChild(i);
                if (shouldSplitOut(*child, fields)) {
                    splitOut.push_back(std::move(child));
                } else {
                    remaining.push_back(std::move(child));
                }
            }
            return {createNorOfNodes(&splitOut), createNorOfNodes(&remaining)};
        }
        case MatchExpression::OR:
        case MatchExpression::INTERNAL_SCHEMA_XOR:
        case MatchExpression::NOT: {
            // We haven't satisfied the split condition, so 'expr' belongs in the remaining match.
            return {nullptr, std::move(expr)};
        }
        default: { MONGO_UNREACHABLE; }
    }
}

}  // namespace

namespace expression {

bool hasExistencePredicateOnPath(const MatchExpression& expr, StringData path) {
    if (expr.getCategory() == MatchExpression::MatchCategory::kLeaf) {
        return (expr.matchType() == MatchExpression::MatchType::EXISTS && expr.path() == path);
    }
    for (size_t i = 0; i < expr.numChildren(); i++) {
        MatchExpression* child = expr.getChild(i);
        if (hasExistencePredicateOnPath(*child, path)) {
            return true;
        }
    }
    return false;
}

bool isSubsetOf(const MatchExpression* lhs, const MatchExpression* rhs) {
    // lhs is the query and rhs is the index.
    invariant(lhs);
    invariant(rhs);

    if (lhs->equivalent(rhs)) {
        return true;
    }

    // $and/$or should be evaluated prior to leaf MatchExpressions. Additionally any recursion
    // should be done through the 'rhs' expression prior to 'lhs'. Swapping the recursion order
    // would cause a comparison like the following to fail as neither the 'a' or 'b' left hand
    // clause would match the $and on the right hand side on their own.
    //     lhs: {a:5, b:5}
    //     rhs: {$or: [{a: 3}, {$and: [{a: 5}, {b: 5}]}]}

    if (rhs->matchType() == MatchExpression::OR) {
        // 'lhs' must match a subset of the documents matched by 'rhs'.
        for (size_t i = 0; i < rhs->numChildren(); i++) {
            if (isSubsetOf(lhs, rhs->getChild(i))) {
                return true;
            }
        }
        return false;
    }

    if (rhs->matchType() == MatchExpression::AND) {
        // 'lhs' must match a subset of the documents matched by each clause of 'rhs'.
        for (size_t i = 0; i < rhs->numChildren(); i++) {
            if (!isSubsetOf(lhs, rhs->getChild(i))) {
                return false;
            }
        }
        return true;
    }

    if (lhs->matchType() == MatchExpression::AND) {
        // At least one clause of 'lhs' must match a subset of the documents matched by 'rhs'.
        for (size_t i = 0; i < lhs->numChildren(); i++) {
            if (isSubsetOf(lhs->getChild(i), rhs)) {
                return true;
            }
        }
        return false;
    }

    if (lhs->matchType() == MatchExpression::OR) {
        // Every clause of 'lhs' must match a subset of the documents matched by 'rhs'.
        for (size_t i = 0; i < lhs->numChildren(); i++) {
            if (!isSubsetOf(lhs->getChild(i), rhs)) {
                return false;
            }
        }
        return true;
    }

    if (lhs->matchType() == MatchExpression::INTERNAL_BUCKET_GEO_WITHIN &&
        rhs->matchType() == MatchExpression::INTERNAL_BUCKET_GEO_WITHIN) {
        auto indexMatchExpression = static_cast<const InternalBucketGeoWithinMatchExpression*>(rhs);

        // {$_internalBucketGeoWithin: {$withinRegion: {$geometry: ...}, field: 'loc' }}
        auto queryInternalBucketGeoWithinObj = lhs->serialize();
        // '$_internalBucketGeoWithin: {$withinRegion: ... , field: 'loc' }'
        auto queryInternalBucketGeoWithinElement = queryInternalBucketGeoWithinObj.firstElement();
        // Confirm that the "field" arguments match before continuing.
        if (queryInternalBucketGeoWithinElement["field"].type() != mongo::String ||
            queryInternalBucketGeoWithinElement["field"].valueStringData() !=
                indexMatchExpression->getField()) {
            return false;
        }
        // {$withinRegion: {$geometry: {type: "Polygon", coords:[...]}}
        auto queryWithinRegionObj = queryInternalBucketGeoWithinElement.Obj();
        // '$withinRegion: {$geometry: {type: "Polygon", coords:[...]}'
        auto queryWithinRegionElement = queryWithinRegionObj.firstElement();
        // {$geometry: {type: "Polygon", coordinates: [...]}}
        auto queryGeometryObj = queryWithinRegionElement.Obj();

        // We only handle $_internalBucketGeoWithin queries that use the $geometry operator.
        if (!queryGeometryObj.hasField("$geometry"))
            return false;

        // geometryElement is '$geometry: {type: ... }'
        auto queryGeometryElement = queryGeometryObj.firstElement();
        MatchDetails* details = nullptr;

        if (GeoMatchExpression::contains(*indexMatchExpression->getGeoContainer(),
                                         GeoExpression::WITHIN,
                                         false,
                                         queryGeometryElement,
                                         details)) {
            // The region described by query is within the region captured by the index.
            // For example, a query over the $geometry for the city of Houston is covered by an
            // index over the $geometry for the entire state of texas. Therefore this index can be
            // used in a potential solution for this query.
            return true;
        }
    }

    if (lhs->matchType() == MatchExpression::GEO && rhs->matchType() == MatchExpression::GEO) {
        // lhs is the query, eg {loc: {$geoWithin: {$geometry: {type: "Polygon", coordinates:
        // [...]}}}} geoWithinObj is {$geoWithin: {$geometry: {type: "Polygon", coordinates:
        // [...]}}} geoWithinElement is '$geoWithin: {$geometry: {type: "Polygon", coordinates:
        // [...]}}' geometryObj is  {$geometry: {type: "Polygon", coordinates: [...]}}
        // geometryElement '$geometry: {type: "Polygon", coordinates: [...]}'

        const GeoMatchExpression* queryMatchExpression =
            static_cast<const GeoMatchExpression*>(lhs);
        // We only handle geoWithin queries
        if (queryMatchExpression->getGeoExpression().getPred() != GeoExpression::WITHIN) {
            return false;
        }
        const GeoMatchExpression* indexMatchExpression =
            static_cast<const GeoMatchExpression*>(rhs);
        auto geoWithinObj = queryMatchExpression->getSerializedRightHandSide();
        auto geoWithinElement = geoWithinObj.firstElement();
        auto geometryObj = geoWithinElement.Obj();

        // More specifically, we only handle geoWithin queries that use the $geometry operator.
        if (!geometryObj.hasField("$geometry")) {
            return false;
        }
        auto geometryElement = geometryObj.firstElement();
        MatchDetails* details = nullptr;

        if (indexMatchExpression->matchesSingleElement(geometryElement, details)) {
            // The region described by query is within the region captured by the index.
            // Therefore this index can be used in a potential solution for this query.
            return true;
        }
    }

    if (ComparisonMatchExpression::isComparisonMatchExpression(rhs)) {
        return _isSubsetOf(lhs, static_cast<const ComparisonMatchExpression*>(rhs));
    }

    if (ComparisonMatchExpressionBase::isInternalExprComparison(rhs->matchType())) {
        return _isSubsetOfInternalExpr(lhs, static_cast<const ComparisonMatchExpressionBase*>(rhs));
    }

    if (rhs->matchType() == MatchExpression::EXISTS) {
        return _isSubsetOf(lhs, static_cast<const ExistsMatchExpression*>(rhs));
    }

    if (rhs->matchType() == MatchExpression::MATCH_IN) {
        return _isSubsetOf(lhs, static_cast<const InMatchExpression*>(rhs));
    }

    return false;
}

// Checks if 'expr' has any children which do not have renaming implemented.
bool hasOnlyRenameableMatchExpressionChildren(const MatchExpression& expr) {
    if (expr.matchType() == MatchExpression::MatchType::EXPRESSION) {
        return true;
    } else if (expr.getCategory() == MatchExpression::MatchCategory::kArrayMatching ||
               expr.getCategory() == MatchExpression::MatchCategory::kOther) {
        return false;
    } else if (expr.getCategory() == MatchExpression::MatchCategory::kLogical) {
        for (size_t i = 0; i < expr.numChildren(); i++) {
            if (!hasOnlyRenameableMatchExpressionChildren(*expr.getChild(i))) {
                return false;
            }
        }
    }
    return true;
}

bool isIndependentOf(const MatchExpression& expr, const std::set<std::string>& pathSet) {
    // Any expression types that do not have renaming implemented cannot have their independence
    // evaluated here. See applyRenamesToExpression().
    if (!hasOnlyRenameableMatchExpressionChildren(expr)) {
        return false;
    }

    auto depsTracker = DepsTracker{};
    expr.addDependencies(&depsTracker);
    return std::none_of(
        depsTracker.fields.begin(), depsTracker.fields.end(), [&pathSet](auto&& field) {
            return pathSet.find(field) != pathSet.end() ||
                std::any_of(pathSet.begin(), pathSet.end(), [&field](auto&& path) {
                       return expression::isPathPrefixOf(field, path) ||
                           expression::isPathPrefixOf(path, field);
                   });
        });
}

bool isOnlyDependentOn(const MatchExpression& expr, const std::set<std::string>& pathSet) {
    // Any expression types that do not have renaming implemented cannot have their independence
    // evaluated here. See applyRenamesToExpression().
    if (!hasOnlyRenameableMatchExpressionChildren(expr)) {
        return false;
    }

    auto depsTracker = DepsTracker{};
    expr.addDependencies(&depsTracker);
    return std::all_of(depsTracker.fields.begin(), depsTracker.fields.end(), [&](auto&& field) {
        return std::any_of(pathSet.begin(), pathSet.end(), [&](auto&& path) {
            return path == field || isPathPrefixOf(path, field);
        });
    });
}

std::pair<unique_ptr<MatchExpression>, unique_ptr<MatchExpression>> splitMatchExpressionBy(
    unique_ptr<MatchExpression> expr,
    const std::set<std::string>& fields,
    const StringMap<std::string>& renames,
    ShouldSplitExprFunc func /*= isIndependentOf */) {
    auto splitExpr = splitMatchExpressionByFunction(std::move(expr), fields, func);
    if (splitExpr.first) {
        applyRenamesToExpression(splitExpr.first.get(), renames);
    }
    return splitExpr;
}

void applyRenamesToExpression(MatchExpression* expr, const StringMap<std::string>& renames) {
    if (expr->matchType() == MatchExpression::MatchType::EXPRESSION) {
        ExprMatchExpression* exprExpr = checked_cast<ExprMatchExpression*>(expr);
        exprExpr->applyRename(renames);
        return;
    }

    if (expr->getCategory() == MatchExpression::MatchCategory::kArrayMatching ||
        expr->getCategory() == MatchExpression::MatchCategory::kOther) {
        return;
    }

    if (expr->getCategory() == MatchExpression::MatchCategory::kLeaf) {
        LeafMatchExpression* leafExpr = checked_cast<LeafMatchExpression*>(expr);
        leafExpr->applyRename(renames);
    }

    for (size_t i = 0; i < expr->numChildren(); ++i) {
        applyRenamesToExpression(expr->getChild(i), renames);
    }
}

void mapOver(MatchExpression* expr, NodeTraversalFunc func, std::string path) {
    if (!expr->path().empty()) {
        if (!path.empty()) {
            path += ".";
        }

        path += expr->path().toString();
    }

    for (size_t i = 0; i < expr->numChildren(); i++) {
        mapOver(expr->getChild(i), func, path);
    }

    func(expr, path);
}

bool isPathPrefixOf(StringData first, StringData second) {
    if (first.size() >= second.size()) {
        return false;
    }

    return second.startsWith(first) && second[first.size()] == '.';
}

bool bidirectionalPathPrefixOf(StringData first, StringData second) {
    return first == second || expression::isPathPrefixOf(first, second) ||
        expression::isPathPrefixOf(second, first);
}
}  // namespace expression
}  // namespace mongo
