// Copyright 2026 Iurii Trukhin (also known as Yuri Trukhin)
// SPDX-License-Identifier: Apache-2.0
#include "app_catalog.h"

#include "proc_util.h"

namespace am {

namespace {

std::vector<CatalogEntry> buildCatalog() {
  std::vector<CatalogEntry> c;
  using E = BrowserEngine;

  // ---- native meeting clients ------------------------------------------
  c.push_back({"zoom", E::None,
               {"zoom", "zoom.real", "ZoomLauncher", "zoomwebview", "ZoomWebviewHost"},
               {"us.zoom.Zoom", "zoom-client", "zoom", "Zoom"},
               {"ZOOM VoiceEngine", "Zoom", "zoom", "Zoom Workplace"},
               {"/opt/zoom/", "/zoom/zoom"}});
  c.push_back({"ms-teams", E::None,
               {"teams-for-linux", "teams", "teams-insiders", "ms-teams", "msteams"},
               {"com.github.IsmaelMartinez.teams_for_linux", "teams-for-linux", "com.microsoft.Teams", "teams"},
               {"teams-for-linux", "Teams for Linux", "Microsoft Teams", "Microsoft Teams - Preview", "Microsoft Teams - Insiders"},
               {"teams-for-linux", "/microsoft/msteams/", "/share/teams/"}});
  c.push_back({"slack", E::None,
               {"slack"},
               {"com.slack.Slack", "slack"},
               {"Slack"},
               {"/lib/slack/", "/slack/slack"}});
  c.push_back({"webex", E::None,
               {"CiscoCollabHost", "CiscoCollabHostCef", "webex", "Webex"},
               {"com.cisco.Webex", "webex"},
               {"Webex", "Cisco Webex", "CiscoCollabHost"},
               {"/opt/Webex/"}});
  c.push_back({"discord", E::None,
               {"Discord", "DiscordCanary", "DiscordPTB", "discord", "discord-canary", "discord-ptb", "vesktop", "Vesktop",
                "webcord", "WebCord", "legcord", "armcord"},
               {"com.discordapp.Discord", "com.discordapp.DiscordCanary", "com.discordapp.DiscordPTB", "dev.vencord.Vesktop",
                "io.github.spacingbat3.webcord", "app.legcord.Legcord", "xyz.armcord.ArmCord", "discord", "discord-canary",
                "vesktop"},
               {"Discord", "Discord Canary", "Discord PTB", "Vesktop", "WebCord", "Legcord", "ArmCord"},
               {"/share/discord/", "/opt/discord/", "/vesktop/", "/webcord/", "/legcord/"}});
  c.push_back({"lark", E::None,
               {"lark", "Lark", "bytedance-lark"},
               {"com.larksuite.Lark", "bytedance-lark", "lark"},
               {"Lark"},
               {"/opt/bytedance/lark/"}});
  c.push_back({"feishu", E::None,
               {"feishu", "Feishu", "bytedance-feishu"},
               {"com.bytedance.Feishu", "bytedance-feishu", "feishu"},
               {"Feishu", "\xE9\xA3\x9E\xE4\xB9\xA6"},  // 飞书
               {"/opt/bytedance/feishu/"}});
  c.push_back({"tencentmeeting", E::None,
               {"wemeetapp", "wemeet", "TencentMeeting"},
               {"com.tencent.wemeet", "wemeet"},
               {"wemeetapp", "TencentMeeting", "VooV Meeting", "\xE8\x85\xBE\xE8\xAE\xAF\xE4\xBC\x9A\xE8\xAE\xAE"},  // 腾讯会议
               {"/opt/wemeet/"}});
  c.push_back({"dingmeeting", E::None,
               {"dingtalk", "DingTalk", "com.alibabainc.dingtalk"},
               {"com.alibabainc.dingtalk", "dingtalk"},
               {"DingTalk", "\xE9\x92\x89\xE9\x92\x89"},  // 钉钉
               {"/com.alibabainc.dingtalk/"}});
  c.push_back({"welinkmeeting", E::None,
               {"welink", "WeLink"},
               {"com.huawei.welink", "welink"},
               {"WeLink"},
               {"/opt/WeLink/", "/welink/"}});
  c.push_back({"whatsapp", E::None,
               {"whatsapp-for-linux", "wasistlos", "zapzap", "whatsdesk"},
               {"com.github.eneshecan.WhatsAppForLinux", "com.rtosta.zapzap", "whatsapp-for-linux", "wasistlos"},
               {"WhatsApp", "WasIstLos", "ZapZap"},
               {}});
  c.push_back({"aircall", E::None, {"aircall", "Aircall"}, {"aircall"}, {"Aircall"}, {"/opt/Aircall/"}});
  c.push_back({"tuple", E::None, {"tuple", "Tuple"}, {"app.tuple.Tuple", "tuple"}, {"Tuple"}, {"/opt/tuple/"}});

  // ---- browsers -------------------------------------------------------------
  // Reported names contain "chrome"/"msedge"/"firefox"/"safari" so the JS
  // classifies them as a browser and applies the "browser" meeting key.
  c.push_back({"chrome", E::Chromium,
               {"chrome", "google-chrome", "google-chrome-stable", "google-chrome-beta", "google-chrome-unstable",
                "google-chrome-canary"},
               {"com.google.Chrome", "com.google.ChromeDev", "google-chrome", "google-chrome-beta", "google-chrome-unstable"},
               {"Google Chrome", "Google Chrome Beta", "Google Chrome Dev", "Google Chrome Canary", "Chrome"},
               {"/opt/google/chrome"}});
  c.push_back({"chrome (chromium)", E::Chromium,
               {"chromium", "chromium-browser", "ungoogled-chromium", "thorium", "thorium-browser", "cromite"},
               {"org.chromium.Chromium", "io.github.ungoogled_software.ungoogled_chromium", "chromium", "chromium-browser",
                "ungoogled-chromium"},
               {"Chromium", "Thorium", "Cromite"},
               {"/lib/chromium/", "/chromium-browser/", "/snap/chromium/"}});
  c.push_back({"msedge", E::Chromium,
               {"msedge", "microsoft-edge", "microsoft-edge-stable", "microsoft-edge-beta", "microsoft-edge-dev"},
               {"com.microsoft.Edge", "com.microsoft.EdgeDev", "microsoft-edge", "microsoft-edge-beta", "microsoft-edge-dev"},
               {"Microsoft Edge", "Microsoft Edge Beta", "Microsoft Edge Dev"},
               {"/opt/microsoft/msedge"}});
  c.push_back({"chrome (brave)", E::Chromium,
               {"brave", "brave-browser", "brave-browser-stable", "brave-browser-beta", "brave-browser-nightly"},
               {"com.brave.Browser", "brave-browser", "brave"},
               {"Brave", "Brave Browser", "Brave Browser Beta", "Brave Browser Nightly"},
               {"/opt/brave.com/"}});
  c.push_back({"chrome (vivaldi)", E::Chromium,
               {"vivaldi-bin", "vivaldi", "vivaldi-stable", "vivaldi-snapshot"},
               {"com.vivaldi.Vivaldi", "vivaldi-stable", "vivaldi"},
               {"Vivaldi"},
               {"/opt/vivaldi"}});
  c.push_back({"chrome (opera)", E::Chromium,
               {"opera", "opera-beta", "opera-developer"},
               {"com.opera.Opera", "opera", "opera-beta"},
               {"Opera", "Opera beta", "Opera developer"},
               {"/opera/opera", "/opera-beta/", "/opera-developer/"}});
  c.push_back({"chrome (yandex)", E::Chromium,
               {"yandex_browser", "yandex-browser", "yandex-browser-stable"},
               {"ru.yandex.Browser", "yandex-browser"},
               {"Yandex Browser", "Yandex"},
               {"/opt/yandex/browser"}});
  c.push_back({"firefox", E::Gecko,
               {"firefox", "firefox-bin", "firefox-esr", "firefox-developer-edition", "firefox-nightly", "firefox-beta"},
               {"org.mozilla.firefox", "org.mozilla.firefox.nightly", "firefox", "firefox-esr", "firefoxdeveloperedition"},
               {"Firefox", "Firefox ESR", "Firefox Developer Edition", "Firefox Nightly", "Firefox Beta", "Nightly"},
               {"/lib/firefox/", "/firefox/firefox", "/lib/firefox-esr/", "/lib/firefox-developer-edition/"}});
  c.push_back({"firefox (librewolf)", E::Gecko,
               {"librewolf", "librewolf-bin"},
               {"io.gitlab.librewolf-community", "librewolf"},
               {"LibreWolf"},
               {"/lib/librewolf/"}});
  c.push_back({"firefox (zen)", E::Gecko,
               {"zen", "zen-bin", "zen-browser"},
               {"app.zen_browser.zen", "zen", "zen-browser"},
               {"Zen", "Zen Browser"},
               {"/opt/zen-browser", "/lib/zen-browser/"}});
  c.push_back({"firefox (floorp)", E::Gecko,
               {"floorp", "floorp-bin"},
               {"one.ablaze.floorp", "floorp"},
               {"Floorp"},
               {"/lib/floorp/"}});
  c.push_back({"firefox (waterfox)", E::Gecko,
               {"waterfox", "waterfox-bin"},
               {"net.waterfox.waterfox", "waterfox"},
               {"Waterfox"},
               {"/lib/waterfox/"}});
  c.push_back({"firefox (mullvad)", E::Gecko,
               {"mullvadbrowser", "mullvad-browser"},
               {"net.mullvad.MullvadBrowser", "mullvad-browser", "mullvadbrowser"},
               {"Mullvad Browser"},
               {"/mullvad-browser/"}});
  c.push_back({"safari (epiphany)", E::WebKit,
               {"epiphany", "epiphany-browser"},
               {"org.gnome.Epiphany", "org.gnome.Epiphany.Devel", "epiphany", "epiphany-browser"},
               {"Epiphany", "GNOME Web"},
               {"/epiphany"}});
  return c;
}

const CatalogEntry* findBy(const std::string& value, std::vector<std::string> CatalogEntry::*field) {
  if (value.empty()) return nullptr;
  for (const CatalogEntry& e : appCatalog()) {
    for (const std::string& candidate : e.*field) {
      if (iequals(candidate, value)) return &e;
    }
  }
  return nullptr;
}

}  // namespace

const std::vector<CatalogEntry>& appCatalog() {
  static const std::vector<CatalogEntry> catalog = buildCatalog();
  return catalog;
}

const CatalogEntry* catalogByBinary(const std::string& binary) { return findBy(binary, &CatalogEntry::binaries); }

const CatalogEntry* catalogByComm(const std::string& comm) {
  if (const CatalogEntry* e = catalogByBinary(comm)) return e;
  constexpr size_t kTaskCommLen = 15;
  if (comm.size() != kTaskCommLen) return nullptr;
  const std::string lc = toLower(comm);
  for (const CatalogEntry& e : appCatalog()) {
    for (const std::string& b : e.binaries) {
      if (b.size() > kTaskCommLen && startsWith(toLower(b), lc)) return &e;
    }
  }
  return nullptr;
}

const CatalogEntry* catalogBySandboxId(const std::string& id) { return findBy(id, &CatalogEntry::sandboxIds); }

const CatalogEntry* catalogByAppName(const std::string& applicationName) {
  return findBy(applicationName, &CatalogEntry::appNames);
}

const CatalogEntry* catalogByPathMarker(const std::string& pathOrArg) {
  if (pathOrArg.empty()) return nullptr;
  for (const CatalogEntry& e : appCatalog()) {
    for (const std::string& marker : e.pathMarkers) {
      if (pathOrArg.find(marker) != std::string::npos) return &e;
    }
  }
  return nullptr;
}

bool isGenericRuntime(const std::string& binary) {
  const std::string b = toLower(binary);
  if (b.empty()) return true;
  if (startsWith(b, "electron")) return true;  // electron, electron36, electron-bin...
  static const char* const kGeneric[] = {"node", "nodejs", "bun", "deno", "python", "python3", "java", "mono",
                                         "dotnet", "bwrap", "flatpak-spawn", "snap", "sh", "bash", "wine",
                                         "wine64", "wine-preloader", "wine64-preloader", "qemu-x86_64"};
  for (const char* g : kGeneric) {
    if (b == g) return true;
  }
  return startsWith(b, "python3.");
}

bool isAudioDaemon(const std::string& binary) {
  static const char* const kDaemons[] = {"pipewire", "pipewire-pulse", "pipewire-media-session", "wireplumber",
                                         "pipewire-aes67", "pipewire-avb", "pulseaudio"};
  for (const char* d : kDaemons) {
    if (binary == d) return true;
  }
  return false;
}

const char* engineName(BrowserEngine engine) {
  switch (engine) {
    case BrowserEngine::Chromium:
      return "chromium";
    case BrowserEngine::Gecko:
      return "gecko";
    case BrowserEngine::WebKit:
      return "webkit";
    case BrowserEngine::None:
      break;
  }
  return "";
}

}  // namespace am
