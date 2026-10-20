// Terminal control panel of bot_mcp, a program of its own (bin/bot_tui): it can come and go while the bots run.
// It talks to each bot through the bot's own socket, run/<name>.sock, and to the program through
// run/.program.sock (JSON-RPC lines, see ServeSocket in mcp.cpp) for the list of bots and the calls that are
// not about one bot (connect, team, disconnect)
#include <protocolCraft/Utilities/Json.hpp>

#include <algorithm>
#include <charconv>
#include <atomic>
#include <chrono>
#include <cmath>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <future>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define NCURSES_NOMACROS
#include <ncurses.h>

using ProtocolCraft::Json::Value;

namespace
{
    const std::string program = ".program";

    /// @brief One request to one bot (or to the program), through its socket. ok is false on an error from the
    /// tool or no answer. method "quit" to the program stops it
    Value AskBot(const std::string& bot, const std::string& tool, Value arguments, const int timeout_s, bool& ok, const std::string& method = "tools/call")
    {
        ok = false;
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, ("../run/" + bot + ".sock").c_str(), sizeof(address.sun_path) - 1);
        timeval limit{ timeout_s, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        {
            close(fd);
            return Value("no answer from " + bot);
        }
        Value request;
        request["jsonrpc"] = "2.0";
        request["id"] = 1;
        request["method"] = method;
        request["params"]["name"] = tool;
        request["params"]["arguments"] = arguments;
        const std::string line = request.Dump() + "\n";
        send(fd, line.data(), line.size(), MSG_NOSIGNAL);
        std::string reply;
        char buffer[65536];
        ssize_t n;
        while (reply.find('\n') == std::string::npos && (n = read(fd, buffer, sizeof(buffer))) > 0)
        {
            reply.append(buffer, n);
        }
        close(fd);
        try
        {
            const Value response = ProtocolCraft::Json::Parse(reply.substr(0, reply.find('\n')));
            const Value& result = response["result"];
            if (method != "tools/call")
            {
                ok = true;
                return result;
            }
            const std::string text = result["content"].get_array().at(0)["text"].get_string();
            ok = !(result.contains("isError") && result["isError"].is_bool() && result["isError"].get<bool>());
            try
            {
                return ProtocolCraft::Json::Parse(text);
            }
            catch (const std::exception&)
            {
                return Value(text);
            }
        }
        catch (const std::exception&)
        {
            return Value("no answer from " + bot);
        }
    }

    /// @brief How many bytes of text fit in cols terminal columns (emojis take two), never cutting a character
    size_t Fit(const std::string& text, const int cols)
    {
        std::mbstate_t state{};
        size_t at = 0;
        int used = 0;
        while (at < text.size())
        {
            wchar_t c;
            const size_t n = std::mbrtowc(&c, text.data() + at, text.size() - at, &state);
            if (n == 0 || n > text.size() - at)
            {
                break;
            }
            const int width = std::max(wcwidth(c), 0);
            if (used + width > cols)
            {
                break;
            }
            used += width;
            at += n;
        }
        return at;
    }

    /// @brief text cut or padded with spaces to exactly cols columns, the last one always a space
    std::string Column(const std::string& text, const int cols)
    {
        std::string out = text.substr(0, Fit(text, cols - 1));
        int used = 0;
        std::mbstate_t state{};
        for (size_t at = 0; at < out.size();)
        {
            wchar_t c;
            const size_t n = std::mbrtowc(&c, out.data() + at, out.size() - at, &state);
            used += std::max(wcwidth(c), 0);
            at += n;
        }
        return out + std::string(std::max(cols - used, 0), ' ');
    }

    std::string Text(const Value& v)
    {
        if (v.is_string())
        {
            return v.get_string();
        }
        return v.is_null() ? "" : v.Dump();
    }

    std::string Field(const Value& o, const std::string& key)
    {
        return o.is_object() && o.contains(key) ? Text(o[key]) : "";
    }

    bool Flag(const Value& o, const std::string& key)
    {
        return o.is_object() && o.contains(key) && o[key].is_bool() && o[key].get<bool>();
    }

    std::string Where(const Value& o)
    {
        if (!o.is_object() || !o.contains("position") || !o["position"].is_object())
        {
            return "?";
        }
        const Value& p = o["position"];
        char text[64];
        std::snprintf(text, sizeof(text), "%.0f %.0f %.0f", p["x"].get_number<double>(), p["y"].get_number<double>(), p["z"].get_number<double>());
        return text;
    }

    std::vector<std::string> Words(const std::string& text)
    {
        std::vector<std::string> words;
        size_t start = 0;
        while (start < text.size())
        {
            const size_t end = std::min(text.find(' ', start), text.size());
            if (end > start)
            {
                words.push_back(text.substr(start, end - start));
            }
            start = end + 1;
        }
        return words;
    }

    /// @brief One bot's neural network (the brain tool's answer to network=bot) as lines of text: each input
    /// with its value, each hidden unit's activity and strongest inputs, each trade's want and strongest hidden units
    /// role: its trade's network (role_network in the answer) instead of its own trade-picking one
    std::vector<std::string> NetworkLines(const std::string& answer, const bool role)
    {
        Value n;
        try
        {
            n = ProtocolCraft::Json::Parse(answer.substr(answer.find(": ") + 2));
            if (role && n.is_object() && n.contains("role_network"))
            {
                const Value inner = n["role_network"];
                n = inner;
            }
        }
        catch (const std::exception&)
        {
            return { answer };
        }
        if (!n.is_object() || !n.contains("w1"))
        {
            return { answer };
        }
        const auto number = [](const Value& v) { return v.is_number() ? v.get_number<double>() : 0.0; };
        char text[512];
        std::vector<std::string> lines;
        if (role)
        {
            std::snprintf(text, sizeof(text), "the %s policy (what %s does next, independently trained)  last action %s  critic value %.2f   (n closes)",
                Field(n, "role").c_str(), Field(n, "bot").c_str(), Field(n, "last_action").empty() ? "-" : Field(n, "last_action").c_str(), number(n["value"]));
        }
        else
        std::snprintf(text, sizeof(text), "%s's trade network  trade %s  generation %s  fitness %s (%s days)  last winner %s  critic value %.2f   (n: its trade's network)",
            Field(n, "bot").c_str(), Field(n, "trade").c_str(), Field(n, "generation").c_str(), Field(n, "fitness").empty() ? "-" : Field(n, "fitness").c_str(),
            Field(n, "days_in_generation").c_str(), Field(n, "champion").empty() ? "-" : Field(n, "champion").c_str(), number(n["value"]));
        lines.push_back(std::string("🧠 ") + text);
        std::string row = "inputs: ";
        std::vector<std::string> inputs;
        for (const Value& i : n["inputs"].get_array())
        {
            inputs.push_back(Field(i, "name"));
            std::snprintf(text, sizeof(text), "%s %.2f", Field(i, "name").c_str(), number(i["value"]));
            row += std::string(row.size() > 8 ? ", " : "") + text;
        }
        lines.push_back(row);
        // The three strongest weights into a unit, "+name" or "-name"
        const auto strongest = [&](const Value& weights, const std::function<std::string(size_t)>& from)
            {
                std::vector<std::pair<double, size_t>> order;
                for (size_t i = 0; i < weights.get_array().size(); ++i)
                {
                    order.push_back({ number(weights[i]), i });
                }
                std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return std::abs(a.first) > std::abs(b.first); });
                std::string out;
                for (size_t k = 0; k < order.size() && k < 3; ++k)
                {
                    std::snprintf(text, sizeof(text), "%s%+.2f %s", out.empty() ? "" : ", ", order[k].first, from(order[k].second).c_str());
                    out += text;
                }
                return out;
            };
        const auto hidden_layers = n.contains("activations") ? n["activations"] : Value(ProtocolCraft::Json::Array());
        for (size_t l = 0; l < hidden_layers.get_array().size(); ++l)
        {
            const auto& hidden = hidden_layers[l].get_array();
            for (size_t h = 0; h < hidden.size(); ++h)
            {
                std::snprintf(text, sizeof(text), "layer %zu hidden %2zu %+.2f <- %s", l + 1, h + 1, number(hidden[h]),
                    strongest(l == 0 ? n["w1"][h] : n["layers"][l - 1]["w"][h], [&](size_t i) {
                        return l == 0 && i < inputs.size() ? inputs[i] : "hidden " + std::to_string(i + 1);
                    }).c_str());
                lines.push_back(text);
            }
        }
        const auto& outputs = n["outputs"].get_array();
        for (size_t o = 0; o < outputs.size(); ++o)
        {
            const double percent = number(outputs[o]["percent"]);
            std::snprintf(text, sizeof(text), "%-26s %5.1f%% %-30s <- %s", Field(outputs[o], "name").c_str(), percent,
                std::string(static_cast<size_t>(percent * 0.3), '#').c_str(),
                strongest(n["w2"][o], [](size_t h) { return "hidden " + std::to_string(h + 1); }).c_str());
            lines.push_back(text);
        }
        return lines;
    }

    int PickList(const std::string& title, const std::vector<std::string>& options)
    {
        int selected = 0;
        while (true)
        {
            const int h = getmaxy(stdscr), w = getmaxx(stdscr);
            werase(stdscr);
            mvwaddstr(stdscr, 0, 0, title.substr(0, Fit(title, w - 1)).c_str());
            const int visible = std::max(1, h - 2);
            const int first = std::max(0, selected - visible + 1);
            for (int i = first; i < static_cast<int>(options.size()) && i < first + visible; ++i)
            {
                if (i == selected) wattron(stdscr, A_REVERSE);
                mvwaddstr(stdscr, i - first + 1, 0, options[i].substr(0, Fit(options[i], w - 1)).c_str());
                if (i == selected) wattroff(stdscr, A_REVERSE);
            }
            wrefresh(stdscr);
            const int key = wgetch(stdscr);
            if (key == 27 || key == 'q') return -1;
            if (key == KEY_UP && selected > 0) --selected;
            if (key == KEY_DOWN && selected + 1 < static_cast<int>(options.size())) ++selected;
            if (key == '\n' || key == '\r' || key == KEY_ENTER) return selected;
        }
    }

    int Number(const std::vector<std::string>& words, const size_t i, const int otherwise)
    {
        try
        {
            return i < words.size() ? std::stoi(words[i]) : otherwise;
        }
        catch (const std::exception&)
        {
            return otherwise;
        }
    }
}

/// @brief Runs until q (the bots keep running) or Q (the program stops them and exits)
int main()
{
    // run/ is next to bin/, where bot_mcp works from
    std::filesystem::current_path(std::filesystem::canonical("/proc/self/exe").parent_path());
    const auto names = []()
        {
            bool ok;
            const Value list = AskBot(program, "bots", Value(ProtocolCraft::Json::Object()), 3, ok);
            std::vector<std::string> found;
            if (ok && list.is_array())
            {
                for (const Value& b : list.get_array())
                {
                    found.push_back(Field(b, "name"));
                }
                return found;
            }
            // No program socket (a program from before it had one): the bot sockets that still take a connection
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator("../run", error))
            {
                const std::string name = entry.path().stem().string();
                if (entry.path().extension() != ".sock" || name == program)
                {
                    continue;
                }
                const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                std::strncpy(address.sun_path, entry.path().c_str(), sizeof(address.sun_path) - 1);
                if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                {
                    found.push_back(name);
                }
                close(fd);
            }
            std::sort(found.begin(), found.end());
            return found;
        };
    const auto call_program = [](const std::string& tool, const Value& arguments)
        {
            bool ok;
            const Value result = AskBot(program, tool, arguments, 600, ok);
            return tool + (ok ? ": " : " failed: ") + Text(result);
        };
    const std::atomic<bool> stop = false;
    // UTF-8 for the emojis
    std::setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    wtimeout(stdscr, 250);

    // Static: a tool call still under way when the panel closes writes its answer here
    static std::mutex mutex;
    static std::string message;
    message = "⏳ starting...";
    std::vector<std::pair<std::string, Value>> statuses;
    std::atomic<bool> done = false;
    // The neural network view (n): whose, and its lines, fetched with the statuses
    static std::string network_bot;
    static bool network_role = false;
    static std::vector<std::string> network_lines;
    network_bot.clear();

    // The statuses, every 2 seconds, off the drawing thread: a busy bot must not freeze the keys
    std::thread poller([&]()
        {
            while (!done && !stop)
            {
                // Every bot at once, 10 seconds each at most: one after the other, a few slow bots held up the rest
                std::vector<std::pair<std::string, std::future<Value>>> asked;
                for (const std::string& name : names())
                {
                    asked.push_back({ name, std::async(std::launch::async, [name]()
                        {
                            bool ok;
                            Value status = AskBot(name, "status", Value(ProtocolCraft::Json::Object()), 10, ok);
                            return ok ? status : Value(ProtocolCraft::Json::Object());
                        }) });
                }
                std::vector<std::pair<std::string, Value>> fresh;
                for (auto& [name, answer] : asked)
                {
                    fresh.push_back({ name, answer.get() });
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    statuses = fresh;
                }
                std::string whose;
                bool role = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    whose = network_bot;
                    role = network_role;
                }
                if (!whose.empty())
                {
                    Value args = ProtocolCraft::Json::Object();
                    args["network"] = whose;
                    std::vector<std::string> fetched = NetworkLines(call_program("brain", args), role);
                    std::lock_guard<std::mutex> lock(mutex);
                    if (network_bot == whose && network_role == role)
                    {
                        network_lines = fetched;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (message == "⏳ starting...")
                    {
                        message = "";
                    }
                }
                for (int i = 0; i < 20 && !done && !stop; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    // A tool call on its own thread: walking, crafting can take minutes
    const auto run_one = [&](const std::string& bot, const std::string& tool, const Value& arguments)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                message = "⏳ " + tool + "...";
            }
            std::thread([bot, tool, arguments, call_program]()
                {
                    std::string text;
                    if (bot.empty())
                    {
                        text = call_program(tool, arguments);
                        text = (text.find(" failed: ") == tool.size() ? "❌ " : "✅ ") + text;
                    }
                    else
                    {
                        bool ok;
                        const Value result = AskBot(bot, tool, arguments, 600, ok);
                        text = std::string(ok ? "✅ " : "❌ ") + tool + (ok ? ": " : " failed: ") + Text(result);
                    }
                    std::lock_guard<std::mutex> lock(mutex);
                    message = text;
                }).detach();
        };
    bool all_bots = false;
    const auto run = [&](const std::string& bot, const std::string& tool, const Value& arguments)
        {
            if (!all_bots || bot.empty())
            {
                run_one(bot, tool, arguments);
                return;
            }
            std::vector<std::string> targets;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& entry : statuses) targets.push_back(entry.first);
            }
            for (const auto& target : targets) run_one(target, tool, arguments);
        };
    const auto ask = [&](const std::string& prompt)
        {
            const int h = getmaxy(stdscr);
            wmove(stdscr, h - 1, 0);
            wclrtoeol(stdscr);
            wattron(stdscr, A_BOLD);
            mvwaddnstr(stdscr, h - 1, 0, prompt.c_str(), getmaxx(stdscr) - 1);
            wattroff(stdscr, A_BOLD);
            echo();
            curs_set(1);
            wtimeout(stdscr, -1);
            char text[400] = {};
            wgetnstr(stdscr, text, sizeof(text) - 1);
            noecho();
            curs_set(0);
            wtimeout(stdscr, 250);
            return std::string(text);
        };

    size_t picked = 0;
    while (!stop)
    {
        std::vector<std::pair<std::string, Value>> bots;
        std::string shown;
        std::vector<std::string> network;
        {
            std::lock_guard<std::mutex> lock(mutex);
            bots = statuses;
            shown = message;
            picked = bots.empty() ? 0 : std::min(picked, bots.size() - 1);
            // The network view follows the picked bot
            if (!network_bot.empty() && !bots.empty() && network_bot != bots[picked].first)
            {
                network_bot = bots[picked].first;
                network_lines = { "🧠 fetching " + network_bot + "'s neural network..." };
            }
            network = network_bot.empty() ? std::vector<std::string>() : network_lines;
        }
        picked = bots.empty() ? 0 : std::min(picked, bots.size() - 1);
        const int h = getmaxy(stdscr);
        const int w = getmaxx(stdscr);
        werase(stdscr);
        const auto line = [&](const int y, const std::string& text, const int attributes = 0)
            {
                if (y < h - 1)
                {
                    wattron(stdscr, attributes);
                    mvwaddstr(stdscr, y, 0, text.substr(0, Fit(text, w - 1)).c_str());
                    wattroff(stdscr, attributes);
                }
            };
        const std::string menu = "🤖 botcraft  " + std::to_string(bots.size()) + (all_bots ? " bots   A [ALL BOTS]" : " bots   A [picked bot]") + "   Enter action list  ↕ pick  a 🚀 autopilot  e 🔧 gear  G ⛏️ gather  m 🔨 make  s 📦 store  f 🌾 farm  T 🗑️ drop items  w 🚶 follow player  W ⏹️ stop following  "
            "l 🧭 find  n 🧠 neural network  d 👋 disconnect  D 🚪 all store and disconnect  R 🔄 all reconnect  c 🔌 connect  C ➕ spawn N bots  t 👥 team  x 💔 disband  : 🛠️ tool  q leave the panel  Q 🛑 stop the program";
        int menu_rows = 0;
        for (size_t at = 0; at < menu.size();)
        {
            size_t count = Fit(menu.substr(at), std::max(1, w - 1));
            if (count == 0) break;
            if (at + count < menu.size())
            {
                const size_t space = menu.rfind(' ', at + count - 1);
                if (space != std::string::npos && space > at) count = space - at + 1;
            }
            line(menu_rows++, menu.substr(at, count), A_REVERSE);
            at += count;
        }
        line(menu_rows, Column("🤖 bot", 13) + Column("📋 state", 23) + Column("🧑 trade", 13) + Column("💗 hp", 7) + Column("🍗 food", 8) + Column("📍 position", 16) + "   🏃 doing", A_BOLD);
        const int rows = std::max(3, h - menu_rows + 1 - (network.empty() ? 12 : 4 + static_cast<int>(network.size())));
        const size_t first = picked >= static_cast<size_t>(rows) ? picked - rows + 1 : 0;
        for (size_t i = first; i < bots.size() && i < first + rows; ++i)
        {
            const Value& b = bots[i].second;
            const std::string flags = std::string(Flag(b, "autopilot") ? "🚀" : "  ") + (Flag(b, "gear") ? "🔧" : "  ");
            const bool answered = b.is_object() && b.contains("state");
            line(menu_rows + 1 + static_cast<int>(i - first), Column(bots[i].first, 13) + Column(answered ? Field(b, "state") : "💤 no answer", 23) + Column(Field(b, "trade"), 13) +
                Column(b.is_object() && b.contains("health") && b["health"].is_number() ? std::to_string(static_cast<int>(std::round(b["health"].get_number<double>()))) : "", 7) + Column(Field(b, "food"), 8) + Column(Where(b), 16) + flags + " " + Field(b, "last_note"), i == picked ? A_REVERSE : 0);
        }
        if (!network.empty())
        {
            int y = menu_rows + rows + 2;
            for (const std::string& text : network)
            {
                line(y++, text);
            }
        }
        else if (!bots.empty())
        {
            const Value& b = bots[picked].second;
            int y = menu_rows + rows + 2;
            std::string inventory;
            if (b.is_object() && b.contains("inventory") && b["inventory"].is_object())
            {
                for (const auto& [item, count] : b["inventory"].get_object())
                {
                    inventory += (inventory.empty() ? "" : ", ") + item + " " + Text(count);
                }
            }
            std::string worn;
            if (b.is_object() && b.contains("equipment") && b["equipment"].is_object())
            {
                for (const auto& [slot, item] : b["equipment"].get_object())
                {
                    if (!Text(item).empty())
                    {
                        worn += (worn.empty() ? "" : ", ") + Text(item);
                    }
                }
            }
            std::string flow;
            if (b.is_object() && b.contains("flow") && b["flow"].is_array())
            {
                const auto& steps = b["flow"].get_array();
                for (size_t i = steps.size() > 6 ? steps.size() - 6 : 0; i < steps.size(); ++i)
                {
                    flow += (flow.empty() ? "" : " | ") + Text(steps[i]);
                }
            }
            for (const std::string& text : {
                "✋ " + bots[picked].first + " holds " + Field(b, "held_item") + "   👕 worn: " + (worn.empty() ? "nothing" : worn),
                "🎒 inventory: " + inventory,
                "🎯 goal: " + Field(b, "gather") + " " + Field(b, "gather_left") + "   🔨 making: " + Field(b, "making") + "   👑 team leader: " + Field(b, "team_leader") +
                    "   💀 deaths: " + Field(b, "deaths") + "   🧱 stuck: " + Field(b, "times_stuck"),
                "🔀 flow: " + flow })
            {
                // Long lines on two rows at most
                const size_t cut = Fit(text, w - 1);
                line(y++, text);
                if (cut < text.size())
                {
                    line(y++, text.substr(cut));
                }
            }
        }
        line(h - 2, shown, A_BOLD);
        wrefresh(stdscr);

        int key = wgetch(stdscr);
        if (key == '\n' || key == '\r' || key == KEY_ENTER)
        {
            const std::vector<std::pair<int, std::string>> actions = {
                { 'a', "Toggle autopilot" }, { 'e', "Toggle gear up" }, { 'G', "Gather" },
                { 'm', "Make" }, { 's', "Store in chests" }, { 'f', "Farm" },
                { 'l', "Find and go" }, { 'd', "Store all and disconnect" }, { ':', "Custom tool" },
                { 'B', "Build castle (whole team)" }, { 'r', "Build roads to mines (whole team)" },
                { 'F', "Create crop farm (near water)" }, { 'M', "Mine ores / open mine as needed" },
                { 'H', "Build shelter" }, { 'V', "Plant castle farms" }, { 'P', "Stock castle animal pens" },
                { 'T', "Drop items (keep armor and tools), wait 10 s" }, { 'w', "Follow a player" }, { 'W', "Stop following, back to work" }
            };
            std::vector<std::string> labels;
            for (const auto& action : actions) labels.push_back(action.second);
            const int selected = PickList(std::string("Action for ") + (all_bots ? "ALL BOTS" : bots.empty() ? "no bot" : bots[picked].first) + " (arrows, Enter; Esc cancels)", labels);
            key = selected < 0 ? ERR : actions[selected].first;
        }
        if (key == ERR)
        {
            continue;
        }
        const std::string bot = bots.empty() ? "" : bots[picked].first;
        const Value& b = bots.empty() ? Value() : bots[picked].second;
        Value args = ProtocolCraft::Json::Object();
        if (key == 'q')
        {
            break;
        }
        else if (key == 'Q')
        {
            const std::string sure = ask("every bot stores everything and the program stops? (y/n) ");
            if (!sure.empty() && (sure[0] == 'y' || sure[0] == 'Y'))
            {
                bool ok;
                AskBot(program, "", Value(), 3, ok, "quit");
                break;
            }
        }
        else if (key == KEY_UP && picked > 0)
        {
            --picked;
        }
        else if (key == KEY_DOWN)
        {
            ++picked;
        }
        else if (key == 'A')
        {
            all_bots = !all_bots;
        }
        else if (key == 'c')
        {
            const std::string who = ask("connect bot named: ");
            if (!who.empty())
            {
                args["name"] = who;
                run("", "connect", args);
            }
        }
        else if (key == 'C')
        {
            const std::string text = ask("spawn how many additional bots (1-100): ");
            if (!text.empty())
            {
                int count = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
                if (parsed.ec == std::errc() && parsed.ptr == text.data() + text.size() && count >= 1 && count <= 100)
                {
                    const std::string base = ask("base name (e.g. Worker -> Worker1, Worker2): ");
                    if (!base.empty())
                    {
                        args["count"] = count;
                        args["base_name"] = base;
                        run("", "spawn", args);
                    }
                }
                else
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    message = "count must be an integer from 1 to 100";
                }
            }
        }
        else if (key == 't')
        {
            const std::vector<std::string> order = Words(ask("team: objective (gather/attack/build/follow) what count [x1 y1 z1 x2 y2 z2]: "));
            if (!order.empty())
            {
                args["objective"] = order[0];
                const std::string what = order.size() > 1 ? order[1] : "";
                args["block"] = what;
                args["type"] = what;
                args["item"] = what;
                args["count"] = Number(order, 2, 16);
                const char* corners[] = { "x1", "y1", "z1", "x2", "y2", "z2" };
                for (size_t i = 0; i < 6 && 3 + i < order.size(); ++i)
                {
                    args[corners[i]] = Number(order, 3 + i, 0);
                }
                run("", "team", args);
            }
        }
        else if (key == 'D')
        {
            const std::string sure = ask("every bot stores everything and disconnects? (y/n) ");
            if (!sure.empty() && (sure[0] == 'y' || sure[0] == 'Y'))
            {
                run("", "disconnect_all", args);
            }
        }
        else if (key == 'R')
        {
            const std::string sure = ask("every bot stores everything, disconnects and joins again (a new build too)? (y/n) ");
            if (!sure.empty() && (sure[0] == 'y' || sure[0] == 'Y'))
            {
                run("", "reconnect_all", args);
            }
        }
        else if (key == 'n')
        {
            std::lock_guard<std::mutex> lock(mutex);
            // Off -> the bot's trade network -> its trade's action network -> off
            if (network_bot.empty())
            {
                network_bot = bot;
                network_role = false;
            }
            else if (!network_role)
            {
                network_role = true;
            }
            else
            {
                network_bot.clear();
            }
            network_lines = { network_bot.empty() ? "" : "🧠 fetching " + network_bot + "'s neural network..." };
        }
        else if (key == 'x')
        {
            args["objective"] = "disband";
            run("", "team", args);
        }
        else if (bot.empty())
        {
            std::lock_guard<std::mutex> lock(mutex);
            message = "🔌 no bot connected: c to connect one, C to spawn N bots";
        }
        else if (key == 'a')
        {
            args["enabled"] = !Flag(b, "autopilot");
            run(bot, "autopilot", args);
        }
        else if (key == 'e')
        {
            args["enabled"] = Flag(b, "autopilot");
            args["gear"] = !Flag(b, "gear");
            run(bot, "autopilot", args);
        }
        else if (key == 'B' || key == 'r' || key == 'V' || key == 'P')
        {
            args["action"] = key == 'B' ? "start" : key == 'r' ? "roads" : key == 'V' ? "farms" : "ranch";
            if (key == 'B')
            {
                const auto at = Words(ask("castle centre x y z (floor level): "));
                if (at.size() != 3) continue;
                bool valid = true;
                for (size_t i = 0; i < 3; ++i)
                {
                    int value;
                    const auto parsed = std::from_chars(at[i].data(), at[i].data() + at[i].size(), value);
                    if (parsed.ec != std::errc() || parsed.ptr != at[i].data() + at[i].size()) { valid = false; break; }
                    args[i == 0 ? "x" : i == 1 ? "y" : "z"] = value;
                }
                if (!valid) continue;
            }
            run("", "castle", args);
        }
        else if (key == 'F')
        {
            args["radius"] = 4;
            run(bot, "make_farm", args);
        }
        else if (key == 'H')
        {
            if (!b.is_object() || !b.contains("position")) continue;
            args["objective"] = "build"; args["plan"] = "shelter"; args["item"] = "cobblestone";
            args["x1"] = b["position"]["x"]; args["y1"] = b["position"]["y"]; args["z1"] = b["position"]["z"];
            run(bot, "team_member", args);
        }
        else if (key == 'G' || key == 'M' || key == 'm')
        {
            std::vector<std::string> what;
            if (key != 'm')
            {
                std::vector<std::string> resources = { "coal_ore", "iron_ore", "copper_ore", "gold_ore", "diamond_ore", "redstone_ore", "lapis_ore", "emerald_ore", "iron_ore,coal_ore" };
                if (key == 'G') resources.insert(resources.end(), { "oak_log", "spruce_log", "birch_log", "cobblestone", "stone", "sand", "gravel", "dirt" });
                resources.push_back("Custom block ids...");
                const int selected = PickList("Resource (arrows, Enter; Esc cancels)", resources);
                if (selected < 0) continue;
                const std::string resource = selected == static_cast<int>(resources.size()) - 1 ? ask("block ids, comma separated: ") : resources[selected];
                const std::string count = ask("count (default 16): ");
                if (resource.empty()) continue;
                int amount = 16;
                if (!count.empty())
                {
                    const auto parsed = std::from_chars(count.data(), count.data() + count.size(), amount);
                    if (parsed.ec != std::errc() || parsed.ptr != count.data() + count.size() || amount < 1)
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        message = "count must be a positive integer";
                        continue;
                    }
                }
                what = { resource, std::to_string(amount) };
            }
            else what = Words(ask("make (item) and count: "));
            if (!what.empty())
            {
                args[key != 'm' ? "gather" : "item"] = what[0];
                args["count"] = Number(what, 1, key != 'm' ? 16 : 1);
                if (key != 'm')
                {
                    args["enabled"] = true;
                }
                run(bot, key != 'm' ? "autopilot" : "make", args);
            }
        }
        else if (key == 's' || key == 'f')
        {
            run(bot, key == 's' ? "store" : "farm", args);
        }
        else if (key == 'l')
        {
            const std::string what = ask("find and go to (structure, e.g. village_plains, mineshaft): ");
            if (!what.empty())
            {
                args["kind"] = "structure";
                args["name"] = what;
                args["go"] = true;
                run(bot, "locate", args);
            }
        }
        else if (key == 'T')
        {
            run(bot, "drop", args);
        }
        else if (key == 'w')
        {
            const std::string player = ask("follow player named: ");
            if (!player.empty())
            {
                args["objective"] = "follow"; args["leader"] = player; args["count"] = 1; args["solo"] = true;
                args["members"] = ProtocolCraft::Json::Array();
                run(bot, "team_member", args);
            }
        }
        else if (key == 'W')
        {
            args["objective"] = "disband"; args["leader"] = ""; args["count"] = 1; args["solo"] = false;
            args["members"] = ProtocolCraft::Json::Array();
            run(bot, "team_member", args);
        }
        else if (key == 'd')
        {
            const std::string sure = ask("store everything and disconnect " + (all_bots ? std::string("ALL BOTS") : bot) + "? (y/n) ");
            if (!sure.empty() && (sure[0] == 'y' || sure[0] == 'Y'))
            {
                // Through the program: it owns the bot processes
                args["bot"] = bot;
                run("", all_bots ? "disconnect_all" : "disconnect", args);
            }
        }
        else if (key == ':')
        {
            const std::string text = ask("tool {json args} (for " + (all_bots ? std::string("ALL BOTS") : bot) + "): ");
            const size_t space = text.find(' ');
            if (!text.empty())
            {
                try
                {
                    if (space != std::string::npos && text.find_first_not_of(' ', space) != std::string::npos)
                    {
                        args = ProtocolCraft::Json::Parse(text.substr(space + 1));
                    }
                    run(bot, text.substr(0, space), args);
                }
                catch (const std::exception& e)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    message = std::string("❌ bad arguments: ") + e.what();
                }
            }
        }
    }
    done = true;
    poller.join();
    endwin();
    // A tool call still under way is dropped: the bot carries on with it
    std::_Exit(0);
}
