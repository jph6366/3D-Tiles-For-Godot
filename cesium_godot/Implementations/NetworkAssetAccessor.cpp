#include "NetworkAssetAccessor.h"
#include "../Models/LocalAssetResponse.h"
#include "../Models/LocalAssetRequest.h"
#include "CesiumAsync/AsyncSystem.h"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/templates/vector.hpp"
#include "godot_cpp/variant/packed_byte_array.hpp"
#include <cstdint>
#include <memory>

#if defined (CESIUM_GD_EXT)
#include <godot_cpp/classes/http_client.hpp>
using namespace godot;
#elif defined(CESIUM_GD_MODULE)
#include "core/io/http_client.h"
#endif

using namespace CesiumAsync;

constexpr uint32_t REQ_DELAY_MICROS = 1000;
constexpr uint32_t EXPECTED_RESPONSE_SIZE_BYTES = 1024 * 120; //120kB expected response
constexpr std::string_view COMPLETED_REQ_EVENT_NAME = "request_completed";

using FutureResult_t = std::shared_ptr<CesiumAsync::IAssetRequest>;

NetworkAssetAccessor::NetworkAssetAccessor()
{
	constexpr size_t maxThreadsPerClient = 16;
	this->m_curlClient.init_client(maxThreadsPerClient);
	// Set all the default headers
	this->m_curlClient.add_default_header({"x-cesium-client", "3D Tiles For Godot"});
	this->m_curlClient.add_default_header({"x-cesium-client-version", "1.0"});
	String godotBuildInfo = Engine::get_singleton()->get_version_info().get("string", "");
	this->m_curlClient.add_default_header({"x-cesium-client-engine", godotBuildInfo.utf8().get_data()});
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> NetworkAssetAccessor::get(const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url, const std::vector<THeader>& headers /*= {}*/)
{
	return asyncSystem.runInWorkerThread([=, this]() {
		return process_request(HTTPClient::METHOD_GET, asyncSystem, url, headers);
	});
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> NetworkAssetAccessor::request(const CesiumAsync::AsyncSystem& asyncSystem, const std::string& verb, const std::string& url, const std::vector<THeader>& headers /*= std::vector<THeader>()*/, const std::span<const std::byte>& contentPayload /*= {}*/)
{
	using HttpMethod_t = HTTPClient::Method;

	constexpr std::array<const char*, 9> STR_METHODS = {
		"GET",
		"POST",
		"PUT",
		"CONNECT",
		"HEAD",
		"DELETE",
		"OPTIONS",
		"TRACE",
		"PATCH"
	};

	constexpr std::array<HttpMethod_t, 9> ENUM_METHODS = {
		HttpMethod_t::METHOD_GET,
		HttpMethod_t::METHOD_POST,
		HttpMethod_t::METHOD_PUT,
		HttpMethod_t::METHOD_CONNECT,
		HttpMethod_t::METHOD_HEAD,
		HttpMethod_t::METHOD_DELETE,
		HttpMethod_t::METHOD_OPTIONS,
		HttpMethod_t::METHOD_TRACE,
		HttpMethod_t::METHOD_PATCH
	};
	static_assert(STR_METHODS.size() == ENUM_METHODS.size());

	int32_t idx = -1;

	for (const char* method : STR_METHODS) {
		if (verb == method) {
			break;
		}
		idx++;
	}

	if (idx < 0 || idx >= ENUM_METHODS.size()) {
		ERR_PRINT(String("Request method with name ") + String(verb.c_str()) + " not found!");
		return asyncSystem.createFuture<FutureResult_t>([=](CesiumAsync::Promise<FutureResult_t> p_promise) {
			p_promise.reject("Request method not found");
			return p_promise;
		});
	}

	//Check what the method is and then request it accordingly
	return asyncSystem.runInWorkerThread([=, this]() {
		return process_request(ENUM_METHODS[idx], asyncSystem, url, headers);
	});
}

void NetworkAssetAccessor::tick() noexcept
{
}

CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> NetworkAssetAccessor::process_request(HTTPClient::Method method, const CesiumAsync::AsyncSystem& asyncSystem, const std::string& url, const std::vector<THeader>& headers /*= {}*/)
{
	CesiumAsync::Promise<FutureResult_t> p_promise = asyncSystem.createPromise<FutureResult_t>();
	CesiumAsync::Future<FutureResult_t> future = p_promise.getFuture();
	this->m_curlClient.send_request(
			url.c_str(),
			method,
			[url, p_promise = std::move(p_promise)](int32_t responseCode, const PackedByteArray &body) {
				if (responseCode >= HTTPClient::ResponseCode::RESPONSE_BAD_REQUEST || responseCode == 0 /* Invalid request will yield 0 */) {
					const String errorMessage = String("The underlying request failed with code: ") + itos(responseCode);
					const char *strPtr = reinterpret_cast<const char *>(body.ptr());
					String bodyStr = strPtr;
					ERR_PRINT(errorMessage + String("\nURL: ") + String(url.c_str()) + String("\nFailed request's body: ") + bodyStr);
					if (responseCode == HTTPClient::ResponseCode::RESPONSE_UNAUTHORIZED) {
						ERR_PRINT("Access to data denied, make sure you're logged into CesiumION and your token has access to the desired asset!");
					}
					std::exception exc(errorMessage.utf8().get_data());
					p_promise.reject(&exc);
					return;
				}

				std::string contentType = "application/octet-stream";
				CesiumAsync::HttpHeaders headers = { { "content-type", contentType } };

				// const char *strPtr = reinterpret_cast<const char *>(body.ptr());
				// printf("%s\n", strPtr);
				
				//Convert the body to a Cesium readable format
				auto assetResponse = std::make_unique<LocalAssetResponse>(
						responseCode,
						contentType,
						headers,
						body);

				auto assetRequest = std::make_shared<LocalAssetRequest>(
						"GET",
						url,
						headers,
						std::move(assetResponse));
				p_promise.resolve(assetRequest);
			},
			headers
	);
	return future;
}
