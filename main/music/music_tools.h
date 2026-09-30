#ifndef MUSIC_TOOLS_H_
#define MUSIC_TOOLS_H_

class McpServer;
class MusicPlayer;

// Restores the music source and play mode from NVS (or menuconfig defaults).
void LoadMusicSettings(MusicPlayer& player);

// Voice-facing tools used by the assistant: self.music.*
void AddMusicTools(McpServer& server);

// Advances the play mode (sequence, repeat all, repeat one, shuffle), saves it and
// shows its name. Used by the KEY button.
void CycleMusicPlayMode();

// Console-only tools for configuring the music source.
void AddMusicUserOnlyTools(McpServer& server);

#endif  // MUSIC_TOOLS_H_
