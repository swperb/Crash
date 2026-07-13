// Settings — user preferences (design doc §4: theme/density/etc. as first-class
// settings), persisted to %LOCALAPPDATA%\Crash\settings.cfg and applied live.
#pragma once

struct AppSettings
{
    int  theme = 0;          // 0 follow system, 1 light, 2 dark
    bool compact = false;    // density
    bool gridDefault = false;// default view mode
    bool showHidden = false;
    bool thumbnails = true;
    bool animations = true;
};

bool LoadSettings(AppSettings& out);
void SaveSettings(const AppSettings& in);
