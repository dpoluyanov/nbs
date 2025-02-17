//
// Copyright 2020 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef GRPC_SRC_CORE_LIB_SECURITY_CREDENTIALS_EXTERNAL_AWS_EXTERNAL_ACCOUNT_CREDENTIALS_H
#define GRPC_SRC_CORE_LIB_SECURITY_CREDENTIALS_EXTERNAL_AWS_EXTERNAL_ACCOUNT_CREDENTIALS_H

#include <grpc/support/port_platform.h>

#include <functional>
#include <memory>
#include <util/generic/string.h>
#include <util/string/cast.h>
#include <vector>

#include "src/core/lib/gprpp/orphanable.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/http/httpcli.h"
#include "src/core/lib/http/parser.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/security/credentials/external/aws_request_signer.h"
#include "src/core/lib/security/credentials/external/external_account_credentials.h"

namespace grpc_core {

class AwsExternalAccountCredentials final : public ExternalAccountCredentials {
 public:
  static RefCountedPtr<AwsExternalAccountCredentials> Create(
      Options options, std::vector<TString> scopes,
      grpc_error_handle* error);

  AwsExternalAccountCredentials(Options options,
                                std::vector<TString> scopes,
                                grpc_error_handle* error);

 private:
  void RetrieveSubjectToken(
      HTTPRequestContext* ctx, const Options& options,
      std::function<void(TString, grpc_error_handle)> cb) override;

  void RetrieveRegion();
  static void OnRetrieveRegion(void* arg, grpc_error_handle error);
  void OnRetrieveRegionInternal(grpc_error_handle error);

  void RetrieveImdsV2SessionToken();
  static void OnRetrieveImdsV2SessionToken(void* arg, grpc_error_handle error);
  void OnRetrieveImdsV2SessionTokenInternal(grpc_error_handle error);

  void RetrieveRoleName();
  static void OnRetrieveRoleName(void* arg, grpc_error_handle error);
  void OnRetrieveRoleNameInternal(grpc_error_handle error);

  void RetrieveSigningKeys();
  static void OnRetrieveSigningKeys(void* arg, grpc_error_handle error);
  void OnRetrieveSigningKeysInternal(grpc_error_handle error);

  void BuildSubjectToken();
  void FinishRetrieveSubjectToken(TString subject_token,
                                  grpc_error_handle error);

  void AddMetadataRequestHeaders(grpc_http_request* request);

  TString audience_;
  OrphanablePtr<HttpRequest> http_request_;

  // Fields of credential source
  TString region_url_;
  TString url_;
  TString regional_cred_verification_url_;
  TString imdsv2_session_token_url_;

  // Information required by request signer
  TString region_;
  TString role_name_;
  TString access_key_id_;
  TString secret_access_key_;
  TString token_;
  TString imdsv2_session_token_;

  std::unique_ptr<AwsRequestSigner> signer_;
  TString cred_verification_url_;

  HTTPRequestContext* ctx_ = nullptr;
  std::function<void(TString, grpc_error_handle)> cb_ = nullptr;
};

}  // namespace grpc_core

#endif  // GRPC_SRC_CORE_LIB_SECURITY_CREDENTIALS_EXTERNAL_AWS_EXTERNAL_ACCOUNT_CREDENTIALS_H
