#include "create_checkpoint.h"

#include <cloud/blockstore/libs/service/context.h>
#include <cloud/blockstore/libs/service/request_helpers.h>
#include <cloud/blockstore/libs/service/service.h>
#include <cloud/storage/core/libs/common/error.h>
#include <cloud/storage/core/libs/diagnostics/logging.h>

#include <library/cpp/protobuf/util/pb_io.h>

namespace NCloud::NBlockStore::NClient {

namespace {

////////////////////////////////////////////////////////////////////////////////

class TCreateCheckpointCommand final
    : public TCommand
{
private:
    TString DiskId;
    TString CheckpointId;

public:
    TCreateCheckpointCommand(IBlockStorePtr client)
        : TCommand(std::move(client))
    {
        Opts.AddLongOption("disk-id", "volume identifier")
            .RequiredArgument("STR")
            .StoreResult(&DiskId);

        Opts.AddLongOption("checkpoint-id", "Checkpoint identifier")
            .RequiredArgument("STR")
            .StoreResult(&CheckpointId);
    }

protected:
    bool DoExecute() override
    {
        if (!Proto && !CheckOpts()) {
            return false;
        }

        auto& input = GetInputStream();
        auto& output = GetOutputStream();

        STORAGE_DEBUG("Reading CreateCheckpoint request");
        auto request = std::make_shared<NProto::TCreateCheckpointRequest>();
        if (Proto) {
            ParseFromTextFormat(input, *request);
        } else {
            request->SetDiskId(DiskId);
            request->SetCheckpointId(CheckpointId);
        }

        STORAGE_DEBUG("Sending CreateCheckpoint request");
        const auto requestId = GetRequestId(*request);
        auto result = WaitFor(ClientEndpoint->CreateCheckpoint(
            MakeIntrusive<TCallContext>(requestId),
            std::move(request)));

        STORAGE_DEBUG("Received CreateCheckpoint response");
        if (Proto) {
            SerializeToTextFormat(result, output);
            return true;
        }

        if (HasError(result)) {
            output << FormatError(result.GetError()) << Endl;
            return false;
        }

        output << "OK" << Endl;
        return true;
    }

private:
    bool CheckOpts() const
    {
        const auto* diskId = ParseResultPtr->FindLongOptParseResult("disk-id");
        if (!diskId) {
            STORAGE_ERROR("Disk id is required");
            return false;
        }

        const auto* checkpointId = ParseResultPtr->FindLongOptParseResult("checkpoint-id");
        if (!checkpointId) {
            STORAGE_ERROR("Checkpoint id is required");
            return false;
        }

        return true;
    }
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

TCommandPtr NewCreateCheckpointCommand(IBlockStorePtr client)
{
    return MakeIntrusive<TCreateCheckpointCommand>(std::move(client));
}

}   // namespace NCloud::NBlockStore::NClient
