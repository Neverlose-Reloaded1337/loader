#include "dll_extractor.h"
#include <vector>

std::string ExtractEmbeddedDll(int resourceId)
{
    // Находим ресурс
    HRSRC hResource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hResource)
        return "";

    // Получаем размер ресурса
    DWORD resourceSize = SizeofResource(nullptr, hResource);
    if (resourceSize == 0)
        return "";

    // Загружаем ресурс
    HGLOBAL hLoadedResource = LoadResource(nullptr, hResource);
    if (!hLoadedResource)
        return "";

    // Блокируем ресурс для получения указателя на данные
    LPVOID pResourceData = LockResource(hLoadedResource);
    if (!pResourceData)
        return "";

    // Получаем путь к временной папке
    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0)
        return "";

    // Создаем уникальное имя файла
    wchar_t tempFileName[MAX_PATH];
    if (GetTempFileNameW(tempPath, L"NL", 0, tempFileName) == 0)
        return "";

    // Записываем DLL во временный файл
    HANDLE hFile = CreateFileW(
        tempFileName,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE)
        return "";

    DWORD bytesWritten = 0;
    bool success = WriteFile(hFile, pResourceData, resourceSize, &bytesWritten, nullptr);
    CloseHandle(hFile);

    if (!success || bytesWritten != resourceSize)
    {
        DeleteFileW(tempFileName);
        return "";
    }

    // Конвертируем путь в ANSI
    char ansiFinalPath[MAX_PATH];
    if (WideCharToMultiByte(CP_ACP, 0, tempFileName, -1, ansiFinalPath, MAX_PATH, nullptr, nullptr) == 0)
    {
        DeleteFileW(tempFileName);
        return "";
    }

    return std::string(ansiFinalPath);
}
