#pragma once

#include <functional>
#include <string>
#include <vector>

using WorkshopLogCallback = std::function<void(const char* message)>;

struct WorkshopItem
{
    std::string  name;
    std::string  githubPath;
    std::string  relative;
    std::string  normalizedName;
    std::string  rawName;
};

bool FetchWorkshopItems(const char*                  githubPrefix,
                        std::vector<WorkshopItem>&   out,
                        WorkshopLogCallback          log);

bool DownloadWorkshopItem(const WorkshopItem&  item,
                          const std::wstring&  localDir,
                          WorkshopLogCallback  log);

bool DeleteWorkshopItem(const WorkshopItem&  item,
                        const std::wstring&  localDir);

bool IsWorkshopItemLocal(const WorkshopItem&  item,
                         const std::wstring&  localDir);
