#pragma once
#include <Windows.h>
#include <string>

// Извлекает встроенную DLL из ресурсов во временный файл
// Возвращает полный путь к извлеченному файлу или пустую строку при ошибке
std::string ExtractEmbeddedDll(int resourceId);
