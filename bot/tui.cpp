// Terminal control panel of bot_mcp (--tui), on its own thread. It talks to each bot through the bot's own
// socket, run/<name>.sock (JSON-RPC lines, see ServeBotSocket in mcp.cpp); the few calls that are not about
// one bot (connect, team) go straight to the program. Its own file: curses macros stay away from mcp.cpp
#include <protocolCraft/Utilities/Json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
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
    /// @brief One tool call to one bot, through its socket. ok is false on an error from the tool or no answer
    Value AskBot(const std::string& bot, const std::string& tool, Value arguments, const int timeout_s, bool& ok)
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
        request["method"] = "tools/call";
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
            std::snprintf(text, sizeof(text), "the %s' network (what %s does next, shared by the trade)  last action %s  critic value %.2f   (n closes)",
                Field(n, "role").c_str(), Field(n, "bot").c_str(), Field(n, "last_action").empty() ? "-" : Field(n, "last_action").c_str(), number(n["value"]));
        }
        else
        std::snprintf(text, sizeof(text), "%s's trade network  trade %s  generation %s  fitness %s (%s days)  last winner %s  critic value %.2f   (n: its trade's network)",
            Field(n, "bot").c_str(), Field(n, "trade").c_str(), Field(n, "generation").c_str(), Field(n, "fitness").empty() ? "-" : Field(n, "fitness").c_str(),
            Field(n, "days_in_generation").c_str(), Field(n, "champion").empty() ? "-" : Field(n, "champion").c_str(), number(n["value"]));
        lines.push_back(text);
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
        const auto& hidden = n["hidden"].get_array();
        for (size_t h = 0; h < hidden.size(); ++h)
        {
            std::snprintf(text, sizeof(text), "hidden %2zu %+.2f  %-36s <- %s", h + 1, number(hidden[h]),
                std::string(static_cast<size_t>(std::abs(number(hidden[h])) * 36), number(hidden[h]) >= 0 ? '+' : '-').c_str(),
                strongest(n["w1"][h], [&](size_t i) { return i < inputs.size() ? inputs[i] : "?"; }).c_str());
            lines.push_back(text);
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

/// @brief Runs until q (then quit() is called) or until stop is set
void RunTui(const std::function<std::vector<std::string>()>& names, const std::function<std::string(const std::string&, const Value&)>& call_program,
    const std::function<void()>& quit, const std::atomic<bool>& stop)
{
    // The terminal itself, not stdin/stdout: those belong to the program (and its logs go to a file meanwhile)
    FILE* tty = fopen("/dev/tty", "r+");
    if (tty == nullptr)
    {
        return;
    }
    SCREEN* screen = newterm(nullptr, tty, tty);
    set_term(screen);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    wtimeout(stdscr, 250);

    // Static: a tool call still under way when the panel closes writes its answer here
    static std::mutex mutex;
    static std::string message;
    message = "starting...";
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
                std::vector<std::pair<std::string, Value>> fresh;
                for (const std::string& name : names())
                {
                    bool ok;
                    Value status = AskBot(name, "status", Value(ProtocolCraft::Json::Object()), 3, ok);
                    fresh.push_back({ name, ok ? status : Value(ProtocolCraft::Json::Object()) });
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
                    if (message == "starting...")
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
    const auto run = [&](const std::string& bot, const std::string& tool, const Value& arguments)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                message = tool + "...";
            }
            std::thread([bot, tool, arguments, call_program]()
                {
                    std::string text;
                    if (bot.empty())
                    {
                        text = call_program(tool, arguments);
                    }
                    else
                    {
                        bool ok;
                        const Value result = AskBot(bot, tool, arguments, 600, ok);
                        text = tool + (ok ? ": " : " failed: ") + Text(result);
                    }
                    std::lock_guard<std::mutex> lock(mutex);
                    message = text;
                }).detach();
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
                network_lines = { "fetching " + network_bot + "'s neural network..." };
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
                    mvwaddnstr(stdscr, y, 0, text.c_str(), w - 1);
                    wattroff(stdscr, attributes);
                }
            };
        line(0, "botcraft  " + std::to_string(bots.size()) + " bots   up/down pick  a autopilot  e gear  G gather  m make  s store  f farm  "
            "l find  n neural network  d disconnect  D all store and disconnect  c connect  t team  x disband  : tool  q quit (bots store and leave)", A_REVERSE);
        char row[512];
        std::snprintf(row, sizeof(row), "%-12s %-22s %6s %4s %-15s %s", "bot", "state", "health", "food", "position", "doing");
        line(1, row, A_BOLD);
        const int rows = std::max(3, h - (network.empty() ? 12 : 4 + static_cast<int>(network.size())));
        const size_t first = picked >= static_cast<size_t>(rows) ? picked - rows + 1 : 0;
        for (size_t i = first; i < bots.size() && i < first + rows; ++i)
        {
            const Value& b = bots[i].second;
            const std::string flags = std::string(Flag(b, "autopilot") ? "A" : "-") + (Flag(b, "gear") ? "G" : "-");
            std::snprintf(row, sizeof(row), "%-12.12s %-22.22s %6.6s %4.4s %-15.15s %s %s", bots[i].first.c_str(),
                (b.is_object() && b.contains("state") ? Field(b, "state") : std::string("no answer")).c_str(), Field(b, "health").c_str(),
                Field(b, "food").c_str(), Where(b).c_str(), flags.c_str(), Field(b, "last_note").c_str());
            line(2 + static_cast<int>(i - first), row, i == picked ? A_REVERSE : 0);
        }
        if (!network.empty())
        {
            int y = 2 + rows + 1;
            for (const std::string& text : network)
            {
                line(y++, text);
            }
        }
        else if (!bots.empty())
        {
            const Value& b = bots[picked].second;
            int y = 2 + rows + 1;
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
                bots[picked].first + " holds " + Field(b, "held_item") + "; worn: " + (worn.empty() ? "nothing" : worn),
                "inventory: " + inventory,
                "goal: " + Field(b, "gather") + " " + Field(b, "gather_left") + "   making: " + Field(b, "making") + "   team leader: " + Field(b, "team_leader") +
                    "   deaths: " + Field(b, "deaths") + "   stuck: " + Field(b, "times_stuck"),
                "flow: " + flow })
            {
                // Long lines on two rows at most
                for (size_t start = 0; start < text.size() && start < 2 * static_cast<size_t>(w - 1); start += w - 1)
                {
                    line(y++, text.substr(start, w - 1));
                }
            }
        }
        line(h - 2, shown, A_BOLD);
        wrefresh(stdscr);

        const int key = wgetch(stdscr);
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
        else if (key == KEY_UP && picked > 0)
        {
            --picked;
        }
        else if (key == KEY_DOWN)
        {
            ++picked;
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
            network_lines = { network_bot.empty() ? "" : "fetching " + network_bot + "'s neural network..." };
        }
        else if (key == 'x')
        {
            args["objective"] = "disband";
            run("", "team", args);
        }
        else if (bot.empty())
        {
            std::lock_guard<std::mutex> lock(mutex);
            message = "no bot connected: c to connect one";
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
        else if (key == 'G' || key == 'm')
        {
            const std::vector<std::string> what = Words(ask(key == 'G' ? "gather (block ids, comma separated) and count: " : "make (item) and count: "));
            if (!what.empty())
            {
                args[key == 'G' ? "gather" : "item"] = what[0];
                args["count"] = Number(what, 1, key == 'G' ? 16 : 1);
                if (key == 'G')
                {
                    args["enabled"] = true;
                }
                run(bot, key == 'G' ? "autopilot" : "make", args);
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
        else if (key == 'd')
        {
            const std::string sure = ask("store everything and disconnect " + bot + "? (y/n) ");
            if (!sure.empty() && (sure[0] == 'y' || sure[0] == 'Y'))
            {
                // Through the program: it owns the bot processes
                args["bot"] = bot;
                run("", "disconnect", args);
            }
        }
        else if (key == ':')
        {
            const std::string text = ask("tool {json args} (for " + bot + "): ");
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
                    message = std::string("bad arguments: ") + e.what();
                }
            }
        }
    }
    done = true;
    poller.join();
    endwin();
    delscreen(screen);
    fclose(tty);
    if (!stop)
    {
        quit();
    }
}
