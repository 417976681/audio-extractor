#pragma once

#include <string>
#include <vector>

// A user-created playlist ("添加到列表" in the UI).
struct Playlist {
    std::string              id;
    std::string              name;
    std::vector<std::string> trackIds;
};

// Runtime configuration for the web player.
struct PlayerConfig {
    int         port = 8080;
    std::string bindAddr = "0.0.0.0";  // all interfaces: reachable from the LAN
    std::string libraryDir;            // where extracted music is stored
    std::string configPath;            // where settings are persisted
    std::string webRoot;               // directory holding index.html etc.
    bool        openBrowser = true;
    bool        manageFirewall = true; // Windows: open inbound TCP <port>
    std::vector<std::string> favorites; // track ids the user starred
    std::vector<Playlist>    playlists; // user-created lists
};

// Description of how the running player can be reached, for the UI.
struct PlayerNetInfo {
    std::string              bindAddr;
    int                      port = 0;
    bool                     lanEnabled = false;  // bound to something non-loopback
    std::string              hostName;
    std::vector<std::string> urls;                // http://ip:port/ candidates
    std::string              firewall;            // ok | not-supported | needs-admin | failed | skipped
    std::string              firewallHint;
};

// Load/save the persisted part of the configuration (library directory).
void  playerLoadConfig(const std::string& path, PlayerConfig& cfg);
void  playerSaveConfig(const std::string& path, const PlayerConfig& cfg);

// Run the player server. Blocks until the server stops. Returns 0 on a clean
// shutdown, non-zero when the port could not be bound. Takes the config by
// value because favourites are edited at runtime and persisted.
int   runPlayerServer(PlayerConfig cfg);
