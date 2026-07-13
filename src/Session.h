// Session — persist/restore the pane + tab layout across runs (design doc §6.5:
// "Session persistence (restore tabs on relaunch) stored as flat JSON").
#pragma once
#include <string>
#include <vector>

struct SessionPane
{
    int activeTab = 0;
    std::vector<std::wstring> tabs;   // folder paths ("" == This PC)
};

struct SessionData
{
    bool  dual = false;
    int   activePane = 0;
    float split = 0.5f;
    std::vector<SessionPane> panes;
};

// Returns true and fills `out` if a valid session file was read.
bool LoadSession(SessionData& out);
void SaveSession(const SessionData& in);
