/*
* This file is part of FiveM: https://fivem.net/
*
* See LICENSE and MENTIONS in the root of the source tree for information
* regarding licensing.
*/

#include "StdInc.h"
#include <ServerIdentityProvider.h>

#define STEAM_API_KEY "04246554B51E8C14ED537C55A4BA4CD7"
#define STEAM_APPID 218

// this imports pplxtasks somewhere?
#define _PPLTASK_ASYNC_LOGGING 0

#include <cpr/cpr.h>
#include <json.hpp>

#include <tbb/concurrent_queue.h>

#include <TcpServerManager.h>
#include <ServerInstanceBase.h>

#include <HttpClient.h>

using json = nlohmann::json;

template<typename Handle, class Class, void(Class::*Callable)()>
void UvCallback(Handle* handle)
{
	(reinterpret_cast<Class*>(handle->data)->*Callable)();
}

static InitFunction initFunction([]()
{
	static fx::ServerInstanceBase* serverInstance;
	static std::unique_ptr<uv_async_t> async;

	static struct SteamIdProvider : public fx::ServerIdentityProviderBase
	{
		HttpClient* httpClient;

		SteamIdProvider()
		{
			httpClient = new HttpClient();
		}

		virtual std::string GetIdentifierPrefix() override
		{
			return "steam";
		}

		virtual int GetVarianceLevel() override
		{
			return 1;
		}

		virtual int GetTrustLevel() override
		{
			return 5;
		}

		virtual void RunAuthentication(const std::shared_ptr<fx::Client>& clientPtr, const std::map<std::string, std::string>& postMap, const std::function<void(boost::optional<std::string>)>& cb) override
		{
			if (!async)
			{
				async = std::make_unique<uv_async_t>();
				async->data = this;

				uv_async_init(serverInstance->GetComponent<net::TcpServerManager>()->GetLoop(), async.get(), UvCallback<uv_async_t, SteamIdProvider, &SteamIdProvider::HandleCallbacks>);
			}

			auto it = postMap.find("authTicket");

			if (it == postMap.end())
			{
				cb({});
				return;
			}

			httpClient->DoGetRequest(
				fmt::format("https://api.steampowered.com/ISteamUserAuth/AuthenticateUserTicket/v1/?key={0}&appid={1}&ticket={2}", STEAM_API_KEY, STEAM_APPID, it->second),
				[this, cb, clientPtr](bool result, const char* data, size_t size)
				{
					std::string response{ data, size };

					m_pendingRequests.push([cb, result, response, clientPtr]()
					{
						try
						{
							if (result)
							{
								json object = json::parse(response)["response"];

								if (object.find("error") != object.end())
								{
									cb({ "Steam rejected authentication: " + object["error"]["errordesc"].get<std::string>() });
									return;
								}

								uint64_t steamId = strtoull(object["params"]["steamid"].get<std::string>().c_str(), nullptr, 10);
								clientPtr->AddIdentifier(fmt::sprintf("steam:%015llx", steamId));
							}

							cb({});
						}
						catch (std::exception& e)
						{
							cb({ fmt::sprintf("SteamIdProvider failure: %s", e.what()) });
						}
					});

					uv_async_send(async.get());
				}
			);
		}

	private:
		void HandleCallbacks()
		{
			std::function<void()> request;

			while (m_pendingRequests.try_pop(request))
			{
				request();
			}
		}

		tbb::concurrent_queue<std::function<void()>> m_pendingRequests;
	} idp;

	fx::ServerInstanceBase::OnServerCreate.Connect([](fx::ServerInstanceBase* instance)
	{
		serverInstance = instance;
	});

	fx::RegisterServerIdentityProvider(&idp);
}, 152);
