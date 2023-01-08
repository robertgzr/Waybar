#include "modules/mpris/mpris.hpp"

#include <fmt/core.h>

#include <optional>
#include <sstream>
#include <string>

extern "C" {
#include <playerctl/playerctl.h>
}

#include <spdlog/spdlog.h>

namespace waybar::modules::mpris {

const std::string DEFAULT_FORMAT = "{player} ({status}): {dynamic}";

Mpris::Mpris(const std::string& id, const Json::Value& config)
    : AModule(config, "mpris", id),
      box_(Gtk::ORIENTATION_HORIZONTAL, 0),
      label_(),
      format_(DEFAULT_FORMAT),
      interval_(0),
      player_("playerctld"),
      manager(),
      player() {
  box_.pack_start(label_);
  box_.set_name(name_);
  event_box_.add(box_);
  event_box_.signal_button_press_event().connect(sigc::mem_fun(*this, &Mpris::handleToggle));

  if (config_["format"].isString()) {
    format_ = config_["format"].asString();
  }
  if (config_["format-playing"].isString()) {
    format_playing_ = config_["format-playing"].asString();
  }
  if (config_["format-paused"].isString()) {
    format_paused_ = config_["format-paused"].asString();
  }
  if (config_["format-stopped"].isString()) {
    format_stopped_ = config_["format-stopped"].asString();
  }
  if (config_["interval"].isUInt()) {
    interval_ = std::chrono::seconds(config_["interval"].asUInt());
  }
  if (config_["player"].isString()) {
    player_ = config_["player"].asString();
  }
  if (config_["ignored-players"].isArray()) {
    for (auto it = config_["ignored-players"].begin(); it != config_["ignored-players"].end();
         ++it) {
      ignored_players_.push_back(it->asString());
    }
  }

  GError* error = nullptr;
  manager = playerctl_player_manager_new(&error);
  if (error) {
    throw std::runtime_error(fmt::format("unable to create MPRIS client: {}", error->message));
  }

  g_object_connect(manager, "signal::name-appeared", G_CALLBACK(onPlayerNameAppeared), this, NULL);
  g_object_connect(manager, "signal::name-vanished", G_CALLBACK(onPlayerNameVanished), this, NULL);

  // allow setting an interval count that triggers periodic refreshes
  if (interval_.count() > 0) {
    thread_ = [this] {
      dp.emit();
      thread_.sleep_for(interval_);
    };
  }

  // trigger initial update
  dp.emit();
}

Mpris::~Mpris() {
  if (manager != NULL) g_object_unref(manager);
  if (player != NULL) g_object_unref(player);
}

auto Mpris::getIcon(const Json::Value& icons, const std::string& key) -> std::string {
  if (icons.isObject()) {
    if (icons[key].isString()) {
      return icons[key].asString();
    } else if (icons["default"].isString()) {
      return icons["default"].asString();
    }
  }
  return "";
}

auto Mpris::onPlayerNameAppeared(PlayerctlPlayerManager* manager, PlayerctlPlayerName* player_name,
                                 gpointer data) -> void {
  Mpris* mpris = static_cast<Mpris*>(data);
  if (!mpris) return;

  spdlog::debug("mpris: name-appeared callback: {}", player_name->name);
  // NOTE: this sleep helps with players what register on the bus when they
  // don't have complete metadata yet and also omit sending a metadata signal
  // when they finally do. e.g. the official spotify client
  // without this delay we never get all metadata due to property caching on the
  // libplayerctl side.
  sleep(1);
  mpris->player = nullptr;
  mpris->dp.emit();
}

auto Mpris::onPlayerNameVanished(PlayerctlPlayerManager* manager, PlayerctlPlayerName* player_name,
                                 gpointer data) -> void {
  Mpris* mpris = static_cast<Mpris*>(data);
  if (!mpris) return;

  spdlog::debug("mpris: name-vanished callback: {}", player_name->name);
  mpris->player = nullptr;
  mpris->dp.emit();
}

auto Mpris::onPlayerPlay(PlayerctlPlayer* player, gpointer data) -> void {
  Mpris* mpris = static_cast<Mpris*>(data);
  if (!mpris) return;

  spdlog::debug("mpris: player-play callback");
  mpris->dp.emit();
}

auto Mpris::onPlayerPause(PlayerctlPlayer* player, gpointer data) -> void {
  Mpris* mpris = static_cast<Mpris*>(data);
  if (!mpris) return;

  spdlog::debug("mpris: player-pause callback");
  mpris->dp.emit();
}

auto Mpris::onPlayerStop(PlayerctlPlayer* player, gpointer data) -> void {
  Mpris* mpris = static_cast<Mpris*>(data);
  if (!mpris) return;

  spdlog::debug("mpris: player-stop callback");
  mpris->event_box_.set_visible(false);
  mpris->dp.emit();
}

auto Mpris::onPlayerMetadata(PlayerctlPlayer* player, GVariant* metadata, gpointer data) -> void {
  Mpris* mpris = static_cast<Mpris*>(data);
  if (!mpris) return;

  spdlog::debug("mpris: player-metadata callback");
  mpris->dp.emit();
}

auto Mpris::getPlayerInfo() -> std::optional<PlayerInfo> {
  if (!player) {
    spdlog::debug("mpris[{}]: no player", player_);
    return std::nullopt;
  }

  GError* error = nullptr;

  char* player_status = nullptr;
  auto player_playback_status = PLAYERCTL_PLAYBACK_STATUS_STOPPED;
  g_object_get(player, "status", &player_status, "playback-status", &player_playback_status, NULL);

  std::string player_name = player_;
  if (player_name == "playerctld") {
    GList* players = playerctl_list_players(&error);
    if (error) {
      auto e = fmt::format("unable to list players: {}", error->message);
      g_error_free(error);
      throw std::runtime_error(e);
    }
    // > get the list of players [..] in order of activity
    // https://github.com/altdesktop/playerctl/blob/b19a71cb9dba635df68d271bd2b3f6a99336a223/playerctl/playerctl-common.c#L248-L249
    players = g_list_first(players);
    if (players) player_name = static_cast<PlayerctlPlayerName*>(players->data)->name;
  }

  if (std::any_of(ignored_players_.begin(), ignored_players_.end(),
                  [&](const std::string& pn) { return player_name == pn; })) {
    spdlog::warn("mpris[{}]: ignoring player update", player_name);
    return std::nullopt;
  }

  // make status lowercase
  player_status[0] = std::tolower(player_status[0]);

  PlayerInfo info = {
      .name = player_name,
      .status = player_playback_status,
      .status_string = player_status,
  };

  if (auto artist_ = playerctl_player_get_artist(player, &error)) {
    if (std::string(artist_).length() > 0) {
      info.artist = Glib::Markup::escape_text(artist_);
      spdlog::debug("mpris[{}]: artist = {}", info.name, artist_);
    }
    g_free(artist_);
  }
  if (error) goto errorexit;

  if (auto album_ = playerctl_player_get_album(player, &error)) {
    if (std::string(album_).length() > 0) {
      info.album = Glib::Markup::escape_text(album_);
      spdlog::debug("mpris[{}]: album = {}", info.name, album_);
    }
    g_free(album_);
  }
  if (error) goto errorexit;

  if (auto title_ = playerctl_player_get_title(player, &error)) {
    if (std::string(title_).length() > 0) {
      info.title = Glib::Markup::escape_text(title_);
      spdlog::debug("mpris[{}]: title = {}", info.name, title_);
    }
    g_free(title_);
  }
  if (error) goto errorexit;

  if (auto length_ = playerctl_player_print_metadata_prop(player, "mpris:length", &error)) {
    std::chrono::microseconds len = std::chrono::microseconds(std::strtol(length_, nullptr, 10));
    g_free(length_);
    if (len.count() > 0) {
      auto len_h = std::chrono::duration_cast<std::chrono::hours>(len);
      auto len_m = std::chrono::duration_cast<std::chrono::minutes>(len - len_h);
      auto len_s = std::chrono::duration_cast<std::chrono::seconds>(len - len_m);
      info.length = len_h.count() > 0 ? fmt::format("{:02}:{:02}:{:02}", len_h.count(),
                                                    len_m.count(), len_s.count())
                                      : fmt::format("{:02}:{:02}", len_m.count(), len_s.count());
      spdlog::debug("mpris[{}]: mpris:length = {}", info.name, *info.length);
    }
  }
  if (error) goto errorexit;

  return info;

errorexit:
  spdlog::error("mpris[{}]: {}", info.name, error->message);
  g_error_free(error);
  return std::nullopt;
}

bool Mpris::handleToggle(GdkEventButton* const& e) {
  GError* error = nullptr;

  auto info = getPlayerInfo();
  if (!info) return false;

  if (e->type == GdkEventType::GDK_BUTTON_PRESS) {
    switch (e->button) {
      case 1:  // left-click
        if (config_["on-click"].isString()) {
          return AModule::handleToggle(e);
        }
        playerctl_player_play_pause(player, &error);
        break;
      case 2:  // middle-click
        if (config_["on-middle-click"].isString()) {
          return AModule::handleToggle(e);
        }
        playerctl_player_previous(player, &error);
        break;
      case 3:  // right-click
        if (config_["on-right-click"].isString()) {
          return AModule::handleToggle(e);
        }
        playerctl_player_next(player, &error);
        break;
    }
  }
  if (error) {
    spdlog::error("mpris[{}]: error running builtin on-click action: {}", (*info).name,
                  error->message);
    g_error_free(error);
    return false;
  }
  return true;
}

auto Mpris::update() -> void {
  if (!player) {
    GError* error = nullptr;
    PlayerctlPlayerName name = {
        .instance = (gchar*)player_.c_str(),
        .source = PLAYERCTL_SOURCE_DBUS_SESSION,
    };
    player = playerctl_player_new_from_name(&name, &error);
    if (error) {
      throw std::runtime_error(
          fmt::format("unable to connect to player {}: {}", player_, error->message));
    }
    g_object_connect(player, "signal::play", G_CALLBACK(onPlayerPlay), this,
                      "signal::pause", G_CALLBACK(onPlayerPause), this, "signal::stop",
                      G_CALLBACK(onPlayerStop), this, "signal::stop", G_CALLBACK(onPlayerStop),
                      this, "signal::metadata", G_CALLBACK(onPlayerMetadata), this, NULL);
  }

  auto opt = getPlayerInfo();
  if (!opt) {
    event_box_.set_visible(false);
    AModule::update();
    return;
  }
  auto info = *opt;

  if (info.status == PLAYERCTL_PLAYBACK_STATUS_STOPPED) {
    spdlog::debug("mpris[{}]: player stopped", info.name);
    event_box_.set_visible(false);
    AModule::update();
    return;
  }

  spdlog::debug("mpris[{}]: running update", info.name);

  // dynamic is the auto-formatted string containing a nice out-of-the-box
  // format text
  std::stringstream dynamic;
  if (info.artist) dynamic << *info.artist;
  if (info.album)
    if (info.artist)
      dynamic << " - ";
    dynamic << *info.album;
  if (info.title)
    if (info.artist || info.album)
      dynamic << " - ";
    dynamic << *info.title;
  if (info.length)
    dynamic << " "
            << "<small>"
            << "[" << *info.length << "]"
            << "</small>";

  // set css class for player status
  if (!lastStatus.empty() && box_.get_style_context()->has_class(lastStatus)) {
    box_.get_style_context()->remove_class(lastStatus);
  }
  if (!box_.get_style_context()->has_class(info.status_string)) {
    box_.get_style_context()->add_class(info.status_string);
  }
  lastStatus = info.status_string;

  // set css class for player name
  if (!lastPlayer.empty() && box_.get_style_context()->has_class(lastPlayer)) {
    box_.get_style_context()->remove_class(lastPlayer);
  }
  if (!box_.get_style_context()->has_class(info.name)) {
    box_.get_style_context()->add_class(info.name);
  }
  lastPlayer = info.name;

  auto formatstr = format_;
  switch (info.status) {
    case PLAYERCTL_PLAYBACK_STATUS_PLAYING:
      if (!format_playing_.empty()) formatstr = format_playing_;
      break;
    case PLAYERCTL_PLAYBACK_STATUS_PAUSED:
      if (!format_paused_.empty()) formatstr = format_paused_;
      break;
    case PLAYERCTL_PLAYBACK_STATUS_STOPPED:
      if (!format_stopped_.empty()) formatstr = format_stopped_;
      break;
  }
  auto label_format =
      fmt::format(formatstr, fmt::arg("player", info.name), fmt::arg("status", info.status_string),
                  fmt::arg("artist", *info.artist), fmt::arg("title", *info.title),
                  fmt::arg("album", *info.album), fmt::arg("length", *info.length),
                  fmt::arg("dynamic", dynamic.str()),
                  fmt::arg("player_icon", getIcon(config_["player-icons"], info.name)),
                  fmt::arg("status_icon", getIcon(config_["status-icons"], info.status_string)));
  label_.set_markup(label_format);

  event_box_.set_visible(true);
  // call parent update
  AModule::update();
}

}  // namespace waybar::modules::mpris