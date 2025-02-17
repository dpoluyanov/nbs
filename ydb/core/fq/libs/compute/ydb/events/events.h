#pragma once

#include <ydb/core/fq/libs/control_plane_storage/proto/yq_internal.pb.h>
#include <ydb/core/fq/libs/events/event_subspace.h>
#include <ydb/core/fq/libs/protos/fq_private.pb.h>
#include <ydb/public/sdk/cpp/client/ydb_query/query.h>
#include <ydb/public/sdk/cpp/client/ydb_types/operation/operation.h>
#include <ydb/public/sdk/cpp/client/ydb_types/status_codes.h>

#include <ydb/library/yql/public/issue/yql_issue.h>

#include <library/cpp/actors/core/event_pb.h>
#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/interconnect/events_local.h>

namespace NFq {

struct TEvYdbCompute {
    // Event ids
    enum EEv : ui32 {
        EvBegin = YqEventSubspaceBegin(NFq::TYqEventSubspace::YdbCompute),

        EvExecuteScriptRequest = EvBegin,
        EvExecuteScriptResponse,
        EvGetOperationRequest,
        EvGetOperationResponse,
        EvFetchScriptResultRequest,
        EvFetchScriptResultResponse,
        EvCancelOperationRequest,
        EvCancelOperationResponse,
        EvForgetOperationRequest,
        EvForgetOperationResponse,
        EvCreateDatabaseRequest,
        EvCreateDatabaseResponse,

        EvInitializerResponse,
        EvExecuterResponse,
        EvStatusTrackerResponse,
        EvResultWriterResponse,
        EvResultSetWriterResponse,
        EvResourcesCleanerResponse,
        EvFinalizerResponse,
        EvStopperResponse,

        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(NActors::TEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(NActors::TEvents::ES_PRIVATE)");

    // Events
    struct TEvExecuteScriptRequest : public NActors::TEventLocal<TEvExecuteScriptRequest, EvExecuteScriptRequest> {
        TEvExecuteScriptRequest(TString sql, TString idempotencyKey)
            : Sql(std::move(sql))
            , IdempotencyKey(std::move(idempotencyKey))
        {}

        TString Sql;
        TString IdempotencyKey;
    };

    struct TEvExecuteScriptResponse : public NActors::TEventLocal<TEvExecuteScriptResponse, EvExecuteScriptResponse> {
        TEvExecuteScriptResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        TEvExecuteScriptResponse(NYdb::TOperation::TOperationId operationId, const TString& executionId)
            : OperationId(operationId)
            , ExecutionId(executionId)
            , Status(NYdb::EStatus::SUCCESS)
        {}

        NYdb::TOperation::TOperationId OperationId;
        TString ExecutionId;
        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvGetOperationRequest : public NActors::TEventLocal<TEvGetOperationRequest, EvGetOperationRequest> {
        explicit TEvGetOperationRequest(const NYdb::TOperation::TOperationId& operationId)
            : OperationId(operationId)
        {}

        NYdb::TOperation::TOperationId OperationId;
    };

    struct TEvGetOperationResponse : public NActors::TEventLocal<TEvGetOperationResponse, EvGetOperationResponse> {
        TEvGetOperationResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        TEvGetOperationResponse(NYdb::NQuery::EExecStatus execStatus, const TVector<Ydb::Query::ResultSetMeta>& resultSetsMeta, NYql::TIssues issues)
            : ExecStatus(execStatus)
            , ResultSetsMeta(resultSetsMeta)
            , Issues(std::move(issues))
            , Status(NYdb::EStatus::SUCCESS)
        {}

        NYdb::NQuery::EExecStatus ExecStatus = NYdb::NQuery::EExecStatus::Unspecified;
        TVector<Ydb::Query::ResultSetMeta> ResultSetsMeta;
        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvFetchScriptResultRequest : public NActors::TEventLocal<TEvFetchScriptResultRequest, EvFetchScriptResultRequest> {
        TEvFetchScriptResultRequest(const NKikimr::NOperationId::TOperationId& operationId, int64_t resultSetId, const TString& fetchToken)
            : OperationId(operationId)
            , ResultSetId(resultSetId)
            , FetchToken(fetchToken)
        {}

        NKikimr::NOperationId::TOperationId OperationId;
        int64_t ResultSetId = 0;
        TString FetchToken;
    };

    struct TEvFetchScriptResultResponse : public NActors::TEventLocal<TEvFetchScriptResultResponse, EvFetchScriptResultResponse> {
        TEvFetchScriptResultResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        explicit TEvFetchScriptResultResponse(NYdb::TResultSet resultSet, const TString& nextFetchToken)
            : ResultSet(std::move(resultSet))
            , NextFetchToken(nextFetchToken)
            , Status(NYdb::EStatus::SUCCESS)
        {}

        TMaybe<NYdb::TResultSet> ResultSet;
        TString NextFetchToken;
        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvCancelOperationRequest : public NActors::TEventLocal<TEvCancelOperationRequest, EvCancelOperationRequest> {
        explicit TEvCancelOperationRequest(const NYdb::TOperation::TOperationId& operationId)
            : OperationId(operationId)
        {}

        NYdb::TOperation::TOperationId OperationId;
    };

    struct TEvCancelOperationResponse : public NActors::TEventLocal<TEvCancelOperationResponse, EvCancelOperationResponse> {
        TEvCancelOperationResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvForgetOperationRequest : public NActors::TEventLocal<TEvForgetOperationRequest, EvForgetOperationRequest> {
        explicit TEvForgetOperationRequest(const NYdb::TOperation::TOperationId& operationId)
            : OperationId(operationId)
        {}

        NYdb::TOperation::TOperationId OperationId;
    };

    struct TEvForgetOperationResponse : public NActors::TEventLocal<TEvForgetOperationResponse, EvForgetOperationResponse> {
        TEvForgetOperationResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvCreateDatabaseRequest : public NActors::TEventLocal<TEvCreateDatabaseRequest, EvCreateDatabaseRequest> {
        TEvCreateDatabaseRequest(const TString& cloudId, const TString& scope)
            : CloudId(cloudId)
            , Scope(scope)
        {}

        TEvCreateDatabaseRequest(const TString& cloudId,
                                 const TString& scope,
                                 const TString& basePath,
                                 const TString& path)
            : CloudId(cloudId)
            , Scope(scope)
            , BasePath(basePath)
            , Path(path)
        {}

        TString CloudId;
        TString Scope;
        TString BasePath;
        TString Path;
    };

    struct TEvCreateDatabaseResponse : public NActors::TEventLocal<TEvCreateDatabaseResponse, EvCreateDatabaseResponse> {
        TEvCreateDatabaseResponse()
        {}

        explicit TEvCreateDatabaseResponse(NYql::TIssues issues)
            : Issues(std::move(issues))
        {}

        TEvCreateDatabaseResponse(const FederatedQuery::Internal::ComputeDatabaseInternal& result)
            : Result(result)
        {}

        FederatedQuery::Internal::ComputeDatabaseInternal Result;
        NYql::TIssues Issues;
    };

    struct TEvInitializerResponse : public NActors::TEventLocal<TEvInitializerResponse, EvInitializerResponse> {
        TEvInitializerResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvExecuterResponse : public NActors::TEventLocal<TEvExecuterResponse, EvExecuterResponse> {
        TEvExecuterResponse(NYdb::TOperation::TOperationId operationId, const TString& executionId)
            : OperationId(operationId)
            , ExecutionId(executionId)
            , Success(true)
        {}

        explicit TEvExecuterResponse(NYql::TIssues issues)
            : Success(false)
            , Issues(std::move(issues))
        {}

        NYdb::TOperation::TOperationId OperationId;
        TString ExecutionId;
        bool Success = true;
        NYql::TIssues Issues;
    };

    struct TEvStatusTrackerResponse : public NActors::TEventLocal<TEvStatusTrackerResponse, EvStatusTrackerResponse> {
        TEvStatusTrackerResponse(NYql::TIssues issues, NYdb::EStatus status, NYdb::NQuery::EExecStatus execStatus)
            : Issues(std::move(issues))
            , Status(status)
            , ExecStatus(execStatus)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
        NYdb::NQuery::EExecStatus ExecStatus;
    };

    struct TEvResultWriterResponse : public NActors::TEventLocal<TEvResultWriterResponse, EvResultWriterResponse> {
        TEvResultWriterResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvResourcesCleanerResponse : public NActors::TEventLocal<TEvResourcesCleanerResponse, EvResourcesCleanerResponse> {
        TEvResourcesCleanerResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvFinalizerResponse : public NActors::TEventLocal<TEvFinalizerResponse, EvFinalizerResponse> {
        TEvFinalizerResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };

    struct TEvStopperResponse : public NActors::TEventLocal<TEvStopperResponse, EvStopperResponse> {
        TEvStopperResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
    };


    struct TEvResultSetWriterResponse : public NActors::TEventLocal<TEvResultSetWriterResponse, EvResultSetWriterResponse> {
        TEvResultSetWriterResponse(NYql::TIssues issues, NYdb::EStatus status)
            : Issues(std::move(issues))
            , Status(status)
        {}

        TEvResultSetWriterResponse(int64_t rowsCount)
            : Status(NYdb::EStatus::SUCCESS)
            , RowsCount(rowsCount)
        {}

        NYql::TIssues Issues;
        NYdb::EStatus Status;
        int64_t RowsCount = 0;
    };
};

}
