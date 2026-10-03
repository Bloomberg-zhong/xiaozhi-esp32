#include "dashboard_tools.h"
#include <cstdio>
#include "application.h"
#include "custom_lcd_display.h"
#include "dashboard_store.h"
#include "dashboard_weather.h"
#include "mcp_server.h"

void AddDashboardTools(CustomLcdDisplay* display) {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.weather.configure",
                       "桌面天气默认按公网IP自动定位城市。city=auto或自动可恢复IP定位；指定上海、成"
                       "都等城市可手动覆盖。空闲查询天气，每30分钟刷新，断网保留上次数据。",
                       PropertyList({Property("city", kPropertyTypeString)}),
                       [](const PropertyList& properties) -> ReturnValue {
                           return std::string(DashboardWeather::Instance().Configure(
                                                  properties["city"].value<std::string>())
                                                  ? "城市已保存，等待空闲时获取真实天气"
                                                  : "城市为空或过长，未保存");
                       });
    mcp_server.AddTool("self.weather.get", "读取桌面已同步的城市天气，以及板载传感器的室内温湿度。",
                       PropertyList(), [](const PropertyList&) -> ReturnValue {
                           auto& store = rlcd_dashboard::DashboardStore::Instance();
                           auto weather = store.GetWeather();
                           auto room = store.GetRoomEnvironment();
                           auto& service = DashboardWeather::Instance();
                           std::string result =
                               std::string("城市来源：") +
                               (service.IsAutomatic() ? "IP自动定位" : "手动设置") +
                               "\n当前城市：" + service.GetCity();
                           if (weather.IsValid())
                               result += "\n城市天气：" + weather.city + " " + weather.condition +
                                         " " + std::to_string(weather.temperature_c) + "°C，湿度" +
                                         std::to_string(weather.humidity_percent) + "%，数据时间" +
                                         weather.updated_at;
                           else
                               result += "\n城市天气尚未同步";
                           if (room) {
                               char text[96];
                               std::snprintf(text, sizeof(text), "\n室内传感器：%.1f°C，湿度%.0f%%",
                                             room->temperature_c, room->humidity_percent);
                               result += text;
                           } else
                               result += "\n室内传感器暂不可用";
                           return result;
                       });
    mcp_server.AddTool(
        "self.weather.update",
        "把外部天气服务查询到的真实当前天气写入设备。调用前必须先查询真实天气，禁止猜测。",
        PropertyList({Property("city", kPropertyTypeString),
                      Property("condition", kPropertyTypeString),
                      Property("temperature_c", kPropertyTypeInteger, -100, 100),
                      Property("humidity_percent", kPropertyTypeInteger, -1, -1, 100),
                      Property("updated_at", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ReturnValue {
            rlcd_dashboard::WeatherData weather;
            weather.city = properties["city"].value<std::string>();
            weather.condition = properties["condition"].value<std::string>();
            weather.temperature_c = properties["temperature_c"].value<int>();
            weather.humidity_percent = properties["humidity_percent"].value<int>();
            weather.updated_at = properties["updated_at"].value<std::string>();
            if (!rlcd_dashboard::DashboardStore::Instance().UpdateWeather(std::move(weather))) {
                return std::string("天气数据无效，未更新");
            }
            return std::string("真实天气已写入设备并持久化");
        });

    mcp_server.AddTool(
        "self.memo.add",
        "添加持久化备忘或闹钟。time 为空表示仅展示；定时提醒使用 HH:MM 或 YYYY-MM-DD HH:MM。",
        PropertyList({Property("content", kPropertyTypeString),
                      Property("time", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ReturnValue {
            auto item = rlcd_dashboard::DashboardStore::Instance().AddReminder(
                properties["time"].value<std::string>(),
                properties["content"].value<std::string>());
            if (!item.has_value()) {
                return std::string("添加失败：时间格式无效、内容为空或备忘已满（最多 8 条）");
            }
            return std::string("已添加备忘，编号 ") + item->id;
        });

    mcp_server.AddTool("self.memo.list", "列出设备中的全部备忘和闹钟。", PropertyList(),
                       [](const PropertyList&) -> ReturnValue {
                           const auto items =
                               rlcd_dashboard::DashboardStore::Instance().GetReminders();
                           if (items.empty()) {
                               return std::string("当前没有备忘");
                           }
                           std::string result = "当前备忘：\n";
                           for (size_t index = 0; index < items.size(); ++index) {
                               result += std::to_string(index + 1) + ". ";
                               if (!items[index].time.empty()) {
                                   result += "[" + items[index].time + "] ";
                               }
                               result += items[index].content + "（" + items[index].id + "）\n";
                           }
                           return result;
                       });

    mcp_server.AddTool(
        "self.memo.done", "完成并删除一条备忘，index 为 self.memo.list 中从 1 开始的序号。",
        PropertyList({Property("index", kPropertyTypeInteger, 1, 8)}),
        [](const PropertyList& properties) -> ReturnValue {
            const auto items = rlcd_dashboard::DashboardStore::Instance().GetReminders();
            const int index = properties["index"].value<int>();
            if (index < 1 || static_cast<size_t>(index) > items.size()) {
                return std::string("备忘序号无效");
            }
            const auto& item = items[static_cast<size_t>(index - 1)];
            if (!rlcd_dashboard::DashboardStore::Instance().RemoveReminder(item.id)) {
                return std::string("删除备忘失败");
            }
            return std::string("已完成：") + item.content;
        });

    mcp_server.AddTool("self.memo.clear", "清空设备中的全部备忘和闹钟。", PropertyList(),
                       [](const PropertyList&) -> ReturnValue {
                           rlcd_dashboard::DashboardStore::Instance().ClearReminders();
                           return std::string("全部备忘已清空");
                       });

    mcp_server.AddTool(
        "self.disp.switch",
        "切换桌面页面。page 为 home/weather（桌面首页）、calendar（日历）或 music（播放器）。",
        PropertyList({Property("page", kPropertyTypeString)}),
        [display](const PropertyList& properties) -> ReturnValue {
            std::string page = properties["page"].value<std::string>();
            using rlcd_dashboard::DashboardPage;
            DashboardPage target;
            if (page == "home" || page == "weather")
                target = DashboardPage::kHome;
            else if (page == "calendar")
                target = DashboardPage::kCalendar;
#if CONFIG_USE_MUSIC_PLAYER
            else if (page == "music")
                target = DashboardPage::kMusic;
#endif
            else
                return std::string("页面无效，请使用 home、calendar 或 music");
            Application::GetInstance().Schedule(
                [display, target]() { display->RequestPage(target); });
            return std::string("已切换页面：") + page;
        });
    mcp_server.AddTool("self.calendar.browse",
                       "日历翻月，offset 为相对当前显示月份的偏移：-1 上月，1 下月。",
                       PropertyList({Property("offset", kPropertyTypeInteger, -12, 12)}),
                       [display](const PropertyList& properties) -> ReturnValue {
                           int offset = properties["offset"].value<int>();
                           Application::GetInstance().Schedule(
                               [display, offset]() { display->BrowseCalendarMonth(offset); });
                           return std::string("已翻到指定月份");
                       });
}
