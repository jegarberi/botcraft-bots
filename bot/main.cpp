#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include "botcraft/Game/ConnectionClient.hpp"
#include "botcraft/Utilities/Logger.hpp"

static std::atomic<bool> stop = false;

// Usage: bot [address] [login]   (empty login "" = Microsoft account auth)
int main(int argc, char* argv[])
{
    const std::string address = argc > 1 ? argv[1] : "192.168.100.78:25565";
    const std::string login = argc > 2 ? argv[2] : "Bot";

    Botcraft::Logger::GetInstance().SetLogLevel(Botcraft::LogLevel::Info);
    Botcraft::Logger::GetInstance().SetFilename("");
    Botcraft::Logger::GetInstance().RegisterThread("main");
    std::signal(SIGINT, [](int) { stop = true; });

    try
    {
        Botcraft::ConnectionClient client;
        client.Connect(address, login);

        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!client.GetShouldBeClosed())
        {
            client.SendChatMessage("Hello from Botcraft!");
        }
        // Stay online until Ctrl-C or the server drops us
        while (!stop && !client.GetShouldBeClosed())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        client.Disconnect();
    }
    catch (const std::exception& e)
    {
        LOG_FATAL("Exception: " << e.what());
        return 1;
    }
    return 0;
}
