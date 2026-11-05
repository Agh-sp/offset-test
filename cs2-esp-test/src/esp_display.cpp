// esp_display.cpp - Phase 2: experimental read-only ESP display (top-down radar) + Insert menu.
// Reads cs2.exe memory (no injection, no writes, no hooks) and renders player
// positions to its own window.
//
// Intended for OFFLINE / PRACTICE use only. READ-ONLY.
//
// Keys:
//   INSERT = toggle menu
//   In menu: Up/Down = navigate, Enter/Space = toggle, Insert = close
//   Outside menu: ESC = exit | + / - = zoom | A = toggle auto-zoom | R = recenter

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include "mem.h"
#include "esp_common.h"

using namespace cs2esp;

namespace {
constexpr float MIN_SCALE = 0.4f;
constexpr float MAX_SCALE = 8.0f;
constexpr float MIN_EXTENT = 500.f;
}

struct App {
    MemReader mem;
    uintptr_t clientBase = 0;
    ListProbe probe{false, 0};
    bool probeOk = false;
    int scanLimit = 96;
    bool running = true;
    HWND hwnd = nullptr;
    int winW = 900, winH = 700;
    float scale = 1.5f;
    bool autoScale = true;

    // Menu state (toggled with INSERT)
    bool menuOpen = false;
    int menuSel = 0;
    bool espEnabled = true;
    bool showBoxes = true;
    bool showHealthBar = true;
    int teamFilter = 0; // 0 = all, 2 = CT, 3 = TERRORIST
};

constexpr int MENU_COUNT = 5;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
                app->winW = static_cast<int>(LOWORD(lParam));
                app->winH = static_cast<int>(HIWORD(lParam));
            }
            return 0;
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN: {
            auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!app) return 0;
            switch (static_cast<int>(wParam)) {
                case VK_INSERT:
                    app->menuOpen = !app->menuOpen;
                    break;
                case VK_ESCAPE:
                    if (!app->menuOpen) PostQuitMessage(0);
                    else app->menuOpen = false;
                    break;
                case VK_ADD: case VK_OEM_PLUS:
                    if (!app->menuOpen) { app->autoScale = false; app->scale *= 1.4f; }
                    break;
                case VK_SUBTRACT: case VK_OEM_MINUS:
                    if (!app->menuOpen) { app->autoScale = false; app->scale /= 1.4f; }
                    break;
                case 'A': case 'a':
                    if (!app->menuOpen) app->autoScale = !app->autoScale;
                    break;
                case 'R': case 'r':
                    if (!app->menuOpen) app->autoScale = true;
                    break;
                case VK_UP:
                    if (app->menuOpen) { app->menuSel = (app->menuSel - 1 + MENU_COUNT) % MENU_COUNT; }
                    break;
                case VK_DOWN:
                    if (app->menuOpen) { app->menuSel = (app->menuSel + 1) % MENU_COUNT; }
                    break;
                case VK_RETURN: case VK_SPACE:
                    if (app->menuOpen) {
                        switch (app->menuSel) {
                            case 0: app->espEnabled = !app->espEnabled; break;
                            case 1: app->showBoxes = !app->showBoxes; break;
                            case 2: app->showHealthBar = !app->showHealthBar; break;
                            case 3: {
                                int t = app->teamFilter;
                                if (t == 0) t = 2; else if (t == 2) t = 3; else if (t == 3) t = 0;
                                app->teamFilter = t;
                                break;
                            }
                            case 4: app->menuOpen = false; break;
                        }
                    }
                    break;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static COLORREF teamColor(int team, bool dormant) {
    COLORREF c;
    switch (team) {
        case 3: c = RGB(255, 70, 70);   break; // TERRORIST -> red
        case 2: c = RGB(70, 130, 255);  break; // CT        -> blue
        case 1: c = RGB(190, 190, 190); break; // spectator -> gray
        case 0: c = RGB(240, 240, 240); break; // unassigned-> white
        default: c = RGB(200, 200, 200);
    }
    if (dormant) {
        c = RGB(GetRValue(c) / 2, GetGValue(c) / 2, GetBValue(c) / 2);
    }
    return c;
}

static const char* teamShort(int team) {
    switch (team) {
        case 3: return "T";
        case 2: return "CT";
        case 1: return "SPEC";
        case 0: return "UNA";
        default: return "?";
    }
}

static void fillCircle(HDC hdc, int cx, int cy, int r, COLORREF color) {
    if (r < 1) r = 1;
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HBRUSH brush = CreateSolidBrush(color);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

static void fillRect(HDC hdc, int x, int y, int w, int h, COLORREF color) {
    RECT rc{ x, y, x + w, y + h };
    HBRUSH brush = CreateSolidBrush(color);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    FillRect(hdc, &rc, brush);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}

static void drawText(HDC hdc, int x, int y, const char* text, COLORREF color) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    TextOutA(hdc, x, y, text, static_cast<int>(strlen(text)));
}

static void renderMenu(HDC hdc, App& app, int w, int h) {
    int panelW = 340, panelH = 200;
    int panelX = (w - panelW) / 2;
    int panelY = (h - panelH) / 2;

    // Background panel
    fillRect(hdc, panelX, panelY, panelW, panelH, RGB(20, 20, 20));

    // Border
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, panelX, panelY, panelX + panelW, panelY + panelH);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);

    // Title
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 0));
    TextOutA(hdc, panelX + 14, panelY + 10, "CS2 ESP MENU (INSERT)", 21);

    // Dynamic labels
    char labels[MENU_COUNT][64];
    snprintf(labels[0], sizeof(labels[0]), "ESP: %s", app.espEnabled ? "ON" : "OFF");
    snprintf(labels[1], sizeof(labels[1]), "Boxes: %s", app.showBoxes ? "ON" : "OFF");
    snprintf(labels[2], sizeof(labels[2]), "Health: %s", app.showHealthBar ? "ON" : "OFF");
    const char* teamStr = (app.teamFilter == 0) ? "All"
                        : (app.teamFilter == 2) ? "CT" : "TERRORIST";
    snprintf(labels[3], sizeof(labels[3]), "Team Filter: %s", teamStr);
    snprintf(labels[4], sizeof(labels[4]), "Close (Insert)");

    for (int i = 0; i < MENU_COUNT; ++i) {
        int y = panelY + 42 + i * 26;
        if (i == app.menuSel) {
            RECT sel{ panelX + 12, y - 4, panelX + panelW - 12, y + 20 };
            FillRect(hdc, &sel, (HBRUSH)GetStockObject(GRAY_BRUSH));
            SetTextColor(hdc, RGB(0, 0, 0));
        } else {
            SetTextColor(hdc, RGB(220, 220, 220));
        }
        TextOutA(hdc, panelX + 22, y, labels[i], static_cast<int>(strlen(labels[i])));
    }

    SetTextColor(hdc, RGB(150, 150, 150));
    TextOutA(hdc, panelX + 14, panelY + panelH - 26,
             "Up/Down: navigate  Enter/Space: toggle", 42);
}

void render(App& app, HDC hdc, int w, int h) {
    RECT rc{ 0, 0, w, h };
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH)); // black background

    int cx = w / 2, cy = h / 2;

    auto local = readLocalPlayer(app.mem, app.clientBase);
    std::optional<float> yaw = readViewYaw(app.mem, app.clientBase);

    // Read entities using the probed layout.
    std::vector<Entity> ents;
    if (app.probeOk && app.espEnabled) {
        ents = readEntities(app.mem, app.probe, app.clientBase, app.scanLimit);
    }

    // Status line (always visible)
    char status[256];
    std::snprintf(status, sizeof(status),
                  "cs2 ESP (read-only) | scale=%.2f u/px | entities=%d | %s",
                  app.scale, static_cast<int>(ents.size()),
                  app.autoScale ? "auto-zoom ON" : "manual zoom");
    drawText(hdc, 10, 10, status, RGB(200, 200, 200));
    if (!local) {
        drawText(hdc, 10, 30, "waiting for local player ...", RGB(200, 60, 60));
    }

    // Menu overlay (INSERT)
    if (app.menuOpen) {
        renderMenu(hdc, app, w, h);
        return; // menu blocks radar interaction while open
    }

    // Range ring (500 units) around local player.
    if (local) {
        int rr = static_cast<int>(500.f * app.scale);
        HPEN pen = CreatePen(PS_DOT, 1, RGB(60, 60, 60));
        HPEN old = (HPEN)SelectObject(hdc, pen);
        Ellipse(hdc, cx - rr, cy - rr, cx + rr, cy + rr);
        SelectObject(hdc, old);
        DeleteObject(pen);
    }

    // Auto-zoom: fit the local player + all entities with padding.
    if (app.autoScale && local) {
        float maxDist = MIN_EXTENT;
        for (const auto& e : ents) {
            float dx = e.origin.x - local->origin.x;
            float dy = e.origin.y - local->origin.y;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d > maxDist) maxDist = d;
        }
        float target = (std::min(w, h) / 2.f) / maxDist;
        if (target > 0.f) {
            app.scale = target;
            if (app.scale < MIN_SCALE) app.scale = MIN_SCALE;
            if (app.scale > MAX_SCALE) app.scale = MAX_SCALE;
        }
    }

    // Other players.
    if (local) {
        for (const auto& e : ents) {
            if (app.teamFilter != 0 && e.team != app.teamFilter) continue;

            float dx = e.origin.x - local->origin.x;
            float dy = e.origin.y - local->origin.y;
            int sx = static_cast<int>(cx + dx * app.scale);
            int sy = static_cast<int>(cy - dy * app.scale);
            if (sx < -40 || sx > w + 40 || sy < -40 || sy > h + 40) continue;

            COLORREF col = teamColor(e.team, e.dormant);

            if (app.showBoxes) {
                int r = 6;
                if (e.team == local->team && e.team != 0) r = 5; // teammates slightly smaller
                fillCircle(hdc, sx, sy, r, col);
            }

            if (app.showHealthBar) {
                int barW = 22, barH = 5;
                int hpPct = e.health;
                if (hpPct < 0) hpPct = 0;
                if (hpPct > 100) hpPct = 100;
                int bx = sx - barW / 2;
                int by = sy - 12;
                fillRect(hdc, bx, by, barW, barH, RGB(60, 0, 0));        // bg
                fillRect(hdc, bx, by, (barW * hpPct) / 100, barH, col); // fill
            }

            char label[64];
            std::snprintf(label, sizeof(label), "%s %d%s",
                          teamShort(e.team), e.health,
                          e.dormant ? " (dormant)" : "");
            drawText(hdc, sx + 8, sy - 7, label, col);
        }

        // Local player at center + facing line.
        fillCircle(hdc, cx, cy, 8, RGB(70, 255, 70)); // local = green
        if (yaw) {
            float rad = (*yaw) * 3.14159265f / 180.f;
            int lx = static_cast<int>(cx + std::cos(rad) * 40.f);
            int ly = static_cast<int>(cy - std::sin(rad) * 40.f);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 230, 80));
            HPEN old = (HPEN)SelectObject(hdc, pen);
            MoveToEx(hdc, cx, cy, nullptr);
            LineTo(hdc, lx, ly);
            SelectObject(hdc, old);
            DeleteObject(pen);
        }

        char info[160];
        std::snprintf(info, sizeof(info),
                      "LOCAL team=%s hp=%d state=%d origin=(%.0f, %.0f, %.0f)",
                      teamShort(local->team), local->health, local->lifeState,
                      local->origin.x, local->origin.y, local->origin.z);
        drawText(hdc, 10, 50, info, RGB(70, 255, 70));
    }
}

int main() {
    AllocConsole();
    // Redirect C runtime streams to the new console.
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    std::printf("=== cs2-esp-display (Phase 2, read-only) ===\n");
    std::printf("Build dump: 14196. Run only in offline/practice matches.\n\n");

    App app;
    if (!app.mem.openByProcessName(L"cs2.exe")) {
        std::printf("ERROR: could not open cs2.exe (is it running?).\n");
        return 1;
    }
    std::printf("Opened cs2.exe (PID %u) read-only.\n", app.mem.pid());

    auto client = app.mem.moduleBase("client.dll");
    if (!client) {
        std::printf("ERROR: client.dll not loaded.\n");
        return 1;
    }
    app.clientBase = *client;

    // Probe entity list layout once at startup.
    int valid = 0;
    auto probed = probeEntityList(app.mem, app.clientBase, app.scanLimit);
    if (probed) {
        app.probe = *probed;
        app.probeOk = true;
        std::printf("Entity list layout: %s stride=%zu\n",
                    app.probe.baseIsPtr ? "listBase-ptr" : "direct-array",
                    app.probe.stride);
    } else {
        std::printf("WARNING: could not resolve entity list layout.\n");
    }

    // Register window class.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Cs2EspDisplay";
    RegisterClassExW(&wc);

    app.hwnd = CreateWindowExW(0, L"Cs2EspDisplay", L"CS2 ESP (read-only radar)",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, app.winW, app.winH,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!app.hwnd) {
        std::printf("ERROR: could not create window.\n");
        return 1;
    }
    SetWindowLongPtrW(app.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));

    std::printf("Window created. INSERT = menu | ESC = exit.\n\n");

    MSG msg{};
    while (app.running) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { app.running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            HDC hdc = GetDC(app.hwnd);
            if (hdc) {
                RECT rc{}; GetClientRect(app.hwnd, &rc);
                render(app, hdc, rc.right - rc.left, rc.bottom - rc.top);
                ReleaseDC(app.hwnd, hdc);
            }
            Sleep(2);
        }
    }

    app.mem.close();
    return 0;
}
