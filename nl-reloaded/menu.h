#pragma once

#include <Windows.h>

using MenuLogCallback = void (*)(const char* message);
using MenuLoaderCallback = int (*)(MenuLogCallback log);

int RunMenu(HINSTANCE instance, MenuLoaderCallback loader);
