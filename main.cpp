// =============================================================
//  LuaSun - engine 2D em arquivo unico
//  C++17 + SDL2 + SDL_image + SDL_ttf + Lua 5.4
//
//  Compilar:
//    g++ -std=c++17 -O2 main.cpp -o luasun -llua5.4 -lSDL2 -lSDL2_image -lSDL2_ttf
// =============================================================

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <algorithm>

#include <lua5.4/lua.hpp>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

namespace
{

// -------------------------------------------------------------
//  Estruturas
// -------------------------------------------------------------

struct Body
{
    std::string name;

    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    float velocityX = 0.0f;
    float velocityY = 0.0f;

    bool dynamic = true;   // sofre gravidade e integra velocidade
    bool solid = true;     // false = sensor (detecta, nao empurra)
    bool grounded = false; // encostou em algo por baixo neste frame
    bool alive = true;
};

struct TextEntry
{
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
    uint64_t lastUsed = 0;
};

struct Engine
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    bool running = false;
    bool insideLoop = false;
    bool sdlReady = false;

    SDL_Color clearColor { 20, 20, 20, 255 };

    // corpos fisicos (indices estaveis, com free list)
    std::vector<Body> bodies;
    std::vector<int> freeSlots;
    std::unordered_map<std::string, int> bodyByName;

    // caches de recursos
    std::unordered_map<std::string, SDL_Texture*> textures;
    std::unordered_map<std::string, TTF_Font*> fonts;
    std::unordered_map<std::string, TextEntry> texts;
    std::unordered_map<std::string, SDL_Scancode> scancodes;

    // input
    Uint8 keyNow[SDL_NUM_SCANCODES];
    Uint8 keyPrev[SDL_NUM_SCANCODES];
    Uint32 mouseNow = 0;
    Uint32 mousePrev = 0;
    int mouseX = 0;
    int mouseY = 0;

    float gravity = 1500.0f;
    int targetFps = 0; // 0 = sem limite manual (vsync cuida disso)

    double elapsed = 0.0;
    double fps = 0.0;
    uint64_t frame = 0;
};

Engine g;

// Buffer de erro: as funcoes auxiliares NUNCA chamam luaL_error
// diretamente (ele faz longjmp e pularia destrutores de std::string).
// Elas devolvem nullptr/false e preenchem este buffer; quem chama
// dispara o erro ja fora de qualquer escopo com objetos C++.
char g_error[512];

void setError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(g_error, sizeof(g_error), format, args);
    va_end(args);
}

// -------------------------------------------------------------
//  Helpers gerais
// -------------------------------------------------------------

inline Uint8 clampColor(lua_Integer value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return static_cast<Uint8>(value);
}

inline bool ensureRenderer(lua_State* lua)
{
    (void)lua;

    if (g.renderer != nullptr)
        return true;

    setError("crie a janela com window.create() antes de desenhar");
    return false;
}

void applyColor(lua_State* lua, int base)
{
    Uint8 red   = clampColor(luaL_optinteger(lua, base + 0, 255));
    Uint8 green = clampColor(luaL_optinteger(lua, base + 1, 255));
    Uint8 blue  = clampColor(luaL_optinteger(lua, base + 2, 255));
    Uint8 alpha = clampColor(luaL_optinteger(lua, base + 3, 255));

    SDL_SetRenderDrawColor(g.renderer, red, green, blue, alpha);
}

// -------------------------------------------------------------
//  Cache de texturas
// -------------------------------------------------------------

SDL_Texture* acquireTexture(const char* path)
{
    auto found = g.textures.find(path);

    if (found != g.textures.end())
        return found->second;

    SDL_Texture* texture = IMG_LoadTexture(g.renderer, path);

    if (!texture)
    {
        setError("falha ao carregar imagem '%s': %s", path, IMG_GetError());
        return nullptr;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    g.textures.emplace(path, texture);

    return texture;
}

// -------------------------------------------------------------
//  Cache de fontes
// -------------------------------------------------------------

TTF_Font* acquireFont(const char* path, int size)
{
    if (size < 1)
        size = 1;

    char key[576];
    snprintf(key, sizeof(key), "%d|%s", size, path);

    auto found = g.fonts.find(key);

    if (found != g.fonts.end())
        return found->second;

    TTF_Font* font = TTF_OpenFont(path, size);

    if (!font)
    {
        setError("falha ao carregar fonte '%s': %s", path, TTF_GetError());
        return nullptr;
    }

    g.fonts.emplace(key, font);

    return font;
}

// -------------------------------------------------------------
//  Cache de texto rasterizado
//  (o codigo antigo abria a fonte e rasterizava a cada frame)
// -------------------------------------------------------------

TextEntry* acquireText(
    const char* text,
    const char* fontPath,
    int size,
    SDL_Color color
)
{
    char key[1024];

    snprintf(
        key, sizeof(key),
        "%d|%02x%02x%02x%02x|%s|%s",
        size, color.r, color.g, color.b, color.a, fontPath, text
    );

    auto found = g.texts.find(key);

    if (found != g.texts.end())
    {
        found->second.lastUsed = g.frame;
        return &found->second;
    }

    TTF_Font* font = acquireFont(fontPath, size);

    if (!font)
        return nullptr;

    SDL_Surface* surface = TTF_RenderUTF8_Blended(font, text, color);

    if (!surface)
    {
        setError("falha ao rasterizar texto: %s", TTF_GetError());
        return nullptr;
    }

    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(g.renderer, surface);

    if (!texture)
    {
        setError("falha ao criar textura de texto: %s", SDL_GetError());
        SDL_FreeSurface(surface);
        return nullptr;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

    TextEntry entry;
    entry.texture = texture;
    entry.width = surface->w;
    entry.height = surface->h;
    entry.lastUsed = g.frame;

    SDL_FreeSurface(surface);

    auto inserted = g.texts.emplace(key, entry);

    return &inserted.first->second;
}

void purgeTextCache()
{
    // roda de tempos em tempos para o cache nao crescer para sempre
    for (auto it = g.texts.begin(); it != g.texts.end(); )
    {
        if (g.frame - it->second.lastUsed > 600)
        {
            SDL_DestroyTexture(it->second.texture);
            it = g.texts.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void clearAssets()
{
    for (auto& pair : g.textures)
        SDL_DestroyTexture(pair.second);

    for (auto& pair : g.texts)
        SDL_DestroyTexture(pair.second.texture);

    for (auto& pair : g.fonts)
        TTF_CloseFont(pair.second);

    g.textures.clear();
    g.texts.clear();
    g.fonts.clear();
}

// -------------------------------------------------------------
//  Corpos / userdata
// -------------------------------------------------------------

const char* BODY_MT = "LuaSun.Body";

struct BodyRef
{
    int id;
};

Body* bodyFromId(int id)
{
    if (id < 0 || id >= static_cast<int>(g.bodies.size()))
        return nullptr;

    Body& body = g.bodies[static_cast<size_t>(id)];

    return body.alive ? &body : nullptr;
}

void pushBody(lua_State* lua, int id)
{
    BodyRef* ref = static_cast<BodyRef*>(
        lua_newuserdatauv(lua, sizeof(BodyRef), 0)
    );

    ref->id = id;

    luaL_setmetatable(lua, BODY_MT);
}

// Aceita userdata OU string (compatibilidade com a API antiga).
Body* checkBody(lua_State* lua, int index)
{
    if (lua_type(lua, index) == LUA_TSTRING)
    {
        const char* name = lua_tostring(lua, index);

        auto found = g.bodyByName.find(name);

        Body* body = (found == g.bodyByName.end())
            ? nullptr
            : bodyFromId(found->second);

        if (!body)
        {
            luaL_error(lua, "o corpo '%s' nao existe", name);
            return nullptr;
        }

        return body;
    }

    BodyRef* ref = static_cast<BodyRef*>(
        luaL_checkudata(lua, index, BODY_MT)
    );

    Body* body = bodyFromId(ref->id);

    if (!body)
    {
        luaL_error(lua, "este corpo ja foi destruido");
        return nullptr;
    }

    return body;
}

inline bool overlaps(const Body& a, const Body& b)
{
    return
        a.x < b.x + b.width &&
        a.x + a.width > b.x &&
        a.y < b.y + b.height &&
        a.y + a.height > b.y;
}

// Resolucao por eixo separado: bem mais estavel que o "menor
// overlap" do codigo original, que travava em quinas e grudava
// o personagem na parede.
void updatePhysics(float deltaTime)
{
    const size_t count = g.bodies.size();

    for (size_t i = 0; i < count; i++)
    {
        Body& body = g.bodies[i];

        if (!body.alive || !body.dynamic)
            continue;

        body.velocityY += g.gravity * deltaTime;
        body.grounded = false;

        // ---- eixo X ----
        body.x += body.velocityX * deltaTime;

        if (body.solid)
        {
            for (size_t j = 0; j < count; j++)
            {
                if (j == i)
                    continue;

                Body& other = g.bodies[j];

                if (!other.alive || other.dynamic || !other.solid)
                    continue;

                if (!overlaps(body, other))
                    continue;

                if (body.velocityX > 0.0f)
                {
                    body.x = other.x - body.width;
                }
                else if (body.velocityX < 0.0f)
                {
                    body.x = other.x + other.width;
                }
                else
                {
                    float left  = (body.x + body.width) - other.x;
                    float right = (other.x + other.width) - body.x;

                    if (left < right)
                        body.x -= left;
                    else
                        body.x += right;
                }

                body.velocityX = 0.0f;
            }
        }

        // ---- eixo Y ----
        body.y += body.velocityY * deltaTime;

        if (body.solid)
        {
            for (size_t j = 0; j < count; j++)
            {
                if (j == i)
                    continue;

                Body& other = g.bodies[j];

                if (!other.alive || other.dynamic || !other.solid)
                    continue;

                if (!overlaps(body, other))
                    continue;

                if (body.velocityY > 0.0f)
                {
                    body.y = other.y - body.height;
                    body.grounded = true;
                }
                else if (body.velocityY < 0.0f)
                {
                    body.y = other.y + other.height;
                }

                body.velocityY = 0.0f;
            }
        }
    }
}

// -------------------------------------------------------------
//  window.*
// -------------------------------------------------------------

int lua_window_create(lua_State* lua)
{
    const char* title = luaL_checkstring(lua, 1);
    int width = static_cast<int>(luaL_checkinteger(lua, 2));
    int height = static_cast<int>(luaL_checkinteger(lua, 3));

    if (width < 1 || height < 1)
        return luaL_error(lua, "tamanho de janela invalido");

    if (g.window != nullptr)
        return luaL_error(lua, "a janela ja foi criada");

    Uint32 flags = SDL_WINDOW_SHOWN;
    bool vsync = true;

    if (lua_istable(lua, 4))
    {
        lua_getfield(lua, 4, "resizable");
        if (lua_toboolean(lua, -1)) flags |= SDL_WINDOW_RESIZABLE;
        lua_pop(lua, 1);

        lua_getfield(lua, 4, "fullscreen");
        if (lua_toboolean(lua, -1)) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        lua_pop(lua, 1);

        lua_getfield(lua, 4, "vsync");
        if (!lua_isnil(lua, -1)) vsync = lua_toboolean(lua, -1);
        lua_pop(lua, 1);
    }

    if (!g.sdlReady)
    {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
            return luaL_error(lua, "falha ao iniciar SDL2: %s", SDL_GetError());

        if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & IMG_INIT_PNG) == 0)
        {
            SDL_Quit();
            return luaL_error(lua, "falha ao iniciar SDL_image: %s", IMG_GetError());
        }

        if (TTF_Init() != 0)
        {
            IMG_Quit();
            SDL_Quit();
            return luaL_error(lua, "falha ao iniciar SDL_ttf: %s", TTF_GetError());
        }

        g.sdlReady = true;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    g.window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        flags
    );

    if (!g.window)
        return luaL_error(lua, "falha ao criar janela: %s", SDL_GetError());

    Uint32 rendererFlags = SDL_RENDERER_ACCELERATED;

    if (vsync)
        rendererFlags |= SDL_RENDERER_PRESENTVSYNC;

    g.renderer = SDL_CreateRenderer(g.window, -1, rendererFlags);

    // fallback: driver de software, em vez de simplesmente falhar
    if (!g.renderer)
        g.renderer = SDL_CreateRenderer(g.window, -1, SDL_RENDERER_SOFTWARE);

    if (!g.renderer)
    {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;

        return luaL_error(lua, "falha ao criar renderer: %s", SDL_GetError());
    }

    // sem isto o canal alpha era simplesmente ignorado
    SDL_SetRenderDrawBlendMode(g.renderer, SDL_BLENDMODE_BLEND);

    memset(g.keyNow, 0, sizeof(g.keyNow));
    memset(g.keyPrev, 0, sizeof(g.keyPrev));

    return 0;
}

int lua_window_title(lua_State* lua)
{
    const char* title = luaL_checkstring(lua, 1);

    if (!g.window)
        return luaL_error(lua, "a janela ainda nao foi criada");

    SDL_SetWindowTitle(g.window, title);

    return 0;
}

int lua_window_size(lua_State* lua)
{
    if (!g.renderer)
        return luaL_error(lua, "a janela ainda nao foi criada");

    int width = 0;
    int height = 0;

    SDL_GetRendererOutputSize(g.renderer, &width, &height);

    lua_pushinteger(lua, width);
    lua_pushinteger(lua, height);

    return 2;
}

int lua_window_background(lua_State* lua)
{
    g.clearColor.r = clampColor(luaL_optinteger(lua, 1, 20));
    g.clearColor.g = clampColor(luaL_optinteger(lua, 2, 20));
    g.clearColor.b = clampColor(luaL_optinteger(lua, 3, 20));
    g.clearColor.a = 255;

    return 0;
}

// -------------------------------------------------------------
//  draw.*
// -------------------------------------------------------------

int lua_draw_rectangle(lua_State* lua)
{
    if (!ensureRenderer(lua))
        return luaL_error(lua, "%s", g_error);

    SDL_Rect rectangle;
    rectangle.x = static_cast<int>(luaL_checknumber(lua, 1));
    rectangle.y = static_cast<int>(luaL_checknumber(lua, 2));
    rectangle.w = static_cast<int>(luaL_checknumber(lua, 3));
    rectangle.h = static_cast<int>(luaL_checknumber(lua, 4));

    applyColor(lua, 5);
    SDL_RenderFillRect(g.renderer, &rectangle);

    return 0;
}

int lua_draw_outline(lua_State* lua)
{
    if (!ensureRenderer(lua))
        return luaL_error(lua, "%s", g_error);

    SDL_Rect rectangle;
    rectangle.x = static_cast<int>(luaL_checknumber(lua, 1));
    rectangle.y = static_cast<int>(luaL_checknumber(lua, 2));
    rectangle.w = static_cast<int>(luaL_checknumber(lua, 3));
    rectangle.h = static_cast<int>(luaL_checknumber(lua, 4));

    applyColor(lua, 5);
    SDL_RenderDrawRect(g.renderer, &rectangle);

    return 0;
}

int lua_draw_circle(lua_State* lua)
{
    if (!ensureRenderer(lua))
        return luaL_error(lua, "%s", g_error);

    int centerX = static_cast<int>(luaL_checknumber(lua, 1));
    int centerY = static_cast<int>(luaL_checknumber(lua, 2));
    int radius = static_cast<int>(luaL_checknumber(lua, 3));

    if (radius <= 0)
        return 0;

    applyColor(lua, 4);

    // antes: O(r^2) chamadas de RenderDrawPoint.
    // agora: O(r) linhas horizontais.
    for (int dy = -radius; dy <= radius; dy++)
    {
        int span = static_cast<int>(
            std::sqrt(static_cast<float>(radius * radius - dy * dy))
        );

        SDL_RenderDrawLine(
            g.renderer,
            centerX - span, centerY + dy,
            centerX + span, centerY + dy
        );
    }

    return 0;
}

int lua_draw_line(lua_State* lua)
{
    if (!ensureRenderer(lua))
        return luaL_error(lua, "%s", g_error);

    int x1 = static_cast<int>(luaL_checknumber(lua, 1));
    int y1 = static_cast<int>(luaL_checknumber(lua, 2));
    int x2 = static_cast<int>(luaL_checknumber(lua, 3));
    int y2 = static_cast<int>(luaL_checknumber(lua, 4));

    applyColor(lua, 5);
    SDL_RenderDrawLine(g.renderer, x1, y1, x2, y2);

    return 0;
}

// draw.image(path, x, y [, width, height, angle, flipX, flipY])
int lua_draw_image(lua_State* lua)
{
    if (!ensureRenderer(lua))
        return luaL_error(lua, "%s", g_error);

    const char* path = luaL_checkstring(lua, 1);

    SDL_Texture* texture = acquireTexture(path);

    if (!texture)
        return luaL_error(lua, "%s", g_error);

    int nativeWidth = 0;
    int nativeHeight = 0;

    SDL_QueryTexture(texture, nullptr, nullptr, &nativeWidth, &nativeHeight);

    SDL_Rect destination;
    destination.x = static_cast<int>(luaL_checknumber(lua, 2));
    destination.y = static_cast<int>(luaL_checknumber(lua, 3));
    destination.w = static_cast<int>(luaL_optnumber(lua, 4, nativeWidth));
    destination.h = static_cast<int>(luaL_optnumber(lua, 5, nativeHeight));

    double angle = luaL_optnumber(lua, 6, 0.0);

    int flip = SDL_FLIP_NONE;

    if (lua_toboolean(lua, 7)) flip |= SDL_FLIP_HORIZONTAL;
    if (lua_toboolean(lua, 8)) flip |= SDL_FLIP_VERTICAL;

    if (angle == 0.0 && flip == SDL_FLIP_NONE)
    {
        SDL_RenderCopy(g.renderer, texture, nullptr, &destination);
    }
    else
    {
        SDL_RenderCopyEx(
            g.renderer,
            texture,
            nullptr,
            &destination,
            angle,
            nullptr,
            static_cast<SDL_RendererFlip>(flip)
        );
    }

    return 0;
}

// draw.sprite(path, sx, sy, sw, sh, x, y [, w, h])
int lua_draw_sprite(lua_State* lua)
{
    if (!ensureRenderer(lua))
        return luaL_error(lua, "%s", g_error);

    const char* path = luaL_checkstring(lua, 1);

    SDL_Texture* texture = acquireTexture(path);

    if (!texture)
        return luaL_error(lua, "%s", g_error);

    SDL_Rect source;
    source.x = static_cast<int>(luaL_checknumber(lua, 2));
    source.y = static_cast<int>(luaL_checknumber(lua, 3));
    source.w = static_cast<int>(luaL_checknumber(lua, 4));
    source.h = static_cast<int>(luaL_checknumber(lua, 5));

    SDL_Rect destination;
    destination.x = static_cast<int>(luaL_checknumber(lua, 6));
    destination.y = static_cast<int>(luaL_checknumber(lua, 7));
    destination.w = static_cast<int>(luaL_optnumber(lua, 8, source.w));
    destination.h = static_cast<int>(luaL_optnumber(lua, 9, source.h));

    SDL_RenderCopy(g.renderer, texture, &source, &destination);

    return 0;
}

int lua_draw_text(lua_State* lua)
{
    if (!ensureRenderer(lua))
        return luaL_error(lua, "%s", g_error);

    const char* text = luaL_checkstring(lua, 1);
    const char* fontPath = luaL_checkstring(lua, 2);

    int size = static_cast<int>(luaL_checkinteger(lua, 3));

    int x = static_cast<int>(luaL_checknumber(lua, 4));
    int y = static_cast<int>(luaL_checknumber(lua, 5));

    SDL_Color color;
    color.r = clampColor(luaL_optinteger(lua, 6, 255));
    color.g = clampColor(luaL_optinteger(lua, 7, 255));
    color.b = clampColor(luaL_optinteger(lua, 8, 255));
    color.a = clampColor(luaL_optinteger(lua, 9, 255));

    if (text[0] == '\0')
        return 0;

    TextEntry* entry = acquireText(text, fontPath, size, color);

    if (!entry)
        return luaL_error(lua, "%s", g_error);

    SDL_Rect destination;
    destination.x = x;
    destination.y = y;
    destination.w = entry->width;
    destination.h = entry->height;

    SDL_RenderCopy(g.renderer, entry->texture, nullptr, &destination);

    return 0;
}

// draw.measure(text, fontPath, size) -> largura, altura
int lua_draw_measure(lua_State* lua)
{
    const char* text = luaL_checkstring(lua, 1);
    const char* fontPath = luaL_checkstring(lua, 2);

    int size = static_cast<int>(luaL_checkinteger(lua, 3));

    TTF_Font* font = acquireFont(fontPath, size);

    if (!font)
        return luaL_error(lua, "%s", g_error);

    int width = 0;
    int height = 0;

    TTF_SizeUTF8(font, text, &width, &height);

    lua_pushinteger(lua, width);
    lua_pushinteger(lua, height);

    return 2;
}

int lua_draw_body(lua_State* lua)
{
    if (!ensureRenderer(lua))
        return luaL_error(lua, "%s", g_error);

    Body* body = checkBody(lua, 1);

    SDL_Rect rectangle;
    rectangle.x = static_cast<int>(body->x);
    rectangle.y = static_cast<int>(body->y);
    rectangle.w = static_cast<int>(body->width);
    rectangle.h = static_cast<int>(body->height);

    if (lua_isnoneornil(lua, 2))
    {
        if (body->dynamic)
            SDL_SetRenderDrawColor(g.renderer, 235, 70, 70, 255);
        else
            SDL_SetRenderDrawColor(g.renderer, 100, 100, 100, 255);
    }
    else
    {
        applyColor(lua, 2);
    }

    SDL_RenderFillRect(g.renderer, &rectangle);

    return 0;
}

int lua_assets_clear(lua_State* lua)
{
    (void)lua;
    clearAssets();
    return 0;
}

// -------------------------------------------------------------
//  input.*
// -------------------------------------------------------------

SDL_Scancode lookupScancode(const char* name)
{
    auto found = g.scancodes.find(name);

    if (found != g.scancodes.end())
        return found->second;

    // SDL_GetScancodeFromName faz busca linear em toda a tabela,
    // entao o resultado fica em cache.
    SDL_Scancode code = SDL_GetScancodeFromName(name);

    g.scancodes.emplace(name, code);

    return code;
}

int keyQuery(lua_State* lua, int mode)
{
    const char* name = luaL_checkstring(lua, 1);

    SDL_Scancode code = lookupScancode(name);

    if (code == SDL_SCANCODE_UNKNOWN)
        return luaL_argerror(lua, 1, "tecla desconhecida");

    bool now = g.keyNow[code] != 0;
    bool previous = g.keyPrev[code] != 0;

    bool result = false;

    if (mode == 0) result = now;                 // segurando
    if (mode == 1) result = now && !previous;    // apertou agora
    if (mode == 2) result = !now && previous;    // soltou agora

    lua_pushboolean(lua, result);

    return 1;
}

int lua_input_key(lua_State* lua)      { return keyQuery(lua, 0); }
int lua_input_pressed(lua_State* lua)  { return keyQuery(lua, 1); }
int lua_input_released(lua_State* lua) { return keyQuery(lua, 2); }

int lua_input_mouse(lua_State* lua)
{
    lua_pushinteger(lua, g.mouseX);
    lua_pushinteger(lua, g.mouseY);
    return 2;
}

int mouseQuery(lua_State* lua, int mode)
{
    int button = static_cast<int>(luaL_optinteger(lua, 1, 1));

    if (button < 1 || button > 5)
        return luaL_argerror(lua, 1, "botao deve estar entre 1 e 5");

    Uint32 mask = SDL_BUTTON(button);

    bool now = (g.mouseNow & mask) != 0;
    bool previous = (g.mousePrev & mask) != 0;

    bool result = (mode == 0) ? now
                : (mode == 1) ? (now && !previous)
                : (!now && previous);

    lua_pushboolean(lua, result);

    return 1;
}

int lua_input_mouse_down(lua_State* lua)     { return mouseQuery(lua, 0); }
int lua_input_mouse_pressed(lua_State* lua)  { return mouseQuery(lua, 1); }
int lua_input_mouse_released(lua_State* lua) { return mouseQuery(lua, 2); }

// -------------------------------------------------------------
//  body.*
// -------------------------------------------------------------

int createBody(lua_State* lua, bool dynamic)
{
    const char* name = luaL_checkstring(lua, 1);

    float x = static_cast<float>(luaL_checknumber(lua, 2));
    float y = static_cast<float>(luaL_checknumber(lua, 3));
    float width = static_cast<float>(luaL_checknumber(lua, 4));
    float height = static_cast<float>(luaL_checknumber(lua, 5));

    if (width <= 0.0f || height <= 0.0f)
        return luaL_error(lua, "o corpo '%s' precisa de largura e altura positivas", name);

    int id;

    auto existing = g.bodyByName.find(name);

    if (existing != g.bodyByName.end() && bodyFromId(existing->second))
    {
        // mesmo nome: reaproveita o slot em vez de vazar um corpo
        id = existing->second;
    }
    else if (!g.freeSlots.empty())
    {
        id = g.freeSlots.back();
        g.freeSlots.pop_back();
    }
    else
    {
        g.bodies.emplace_back();
        id = static_cast<int>(g.bodies.size()) - 1;
    }

    Body& body = g.bodies[static_cast<size_t>(id)];

    body = Body();              // zera TUDO (o codigo antigo deixava
    body.name = name;           // velocityX/Y com lixo de memoria)
    body.x = x;
    body.y = y;
    body.width = width;
    body.height = height;
    body.dynamic = dynamic;

    g.bodyByName[name] = id;

    pushBody(lua, id);

    return 1;
}

int lua_body_dynamic(lua_State* lua) { return createBody(lua, true); }
int lua_body_static(lua_State* lua)  { return createBody(lua, false); }

int lua_body_get(lua_State* lua)
{
    const char* name = luaL_checkstring(lua, 1);

    auto found = g.bodyByName.find(name);

    if (found == g.bodyByName.end() || !bodyFromId(found->second))
    {
        lua_pushnil(lua);
        return 1;
    }

    pushBody(lua, found->second);

    return 1;
}

int lua_body_collides(lua_State* lua)
{
    Body* a = checkBody(lua, 1);
    Body* b = checkBody(lua, 2);

    lua_pushboolean(lua, overlaps(*a, *b));

    return 1;
}

int lua_body_destroy(lua_State* lua)
{
    Body* body = checkBody(lua, 1);

    auto found = g.bodyByName.find(body->name);

    if (found != g.bodyByName.end())
    {
        g.freeSlots.push_back(found->second);
        g.bodyByName.erase(found);
    }

    body->alive = false;
    body->name.clear();

    return 0;
}

int lua_body_move(lua_State* lua)
{
    Body* body = checkBody(lua, 1);

    body->x += static_cast<float>(luaL_checknumber(lua, 2));
    body->y += static_cast<float>(luaL_checknumber(lua, 3));

    return 0;
}

int lua_body_set_position(lua_State* lua)
{
    Body* body = checkBody(lua, 1);

    body->x = static_cast<float>(luaL_checknumber(lua, 2));
    body->y = static_cast<float>(luaL_checknumber(lua, 3));

    return 0;
}

int lua_body_set_velocity(lua_State* lua)
{
    Body* body = checkBody(lua, 1);

    body->velocityX = static_cast<float>(luaL_checknumber(lua, 2));
    body->velocityY = static_cast<float>(luaL_checknumber(lua, 3));

    return 0;
}

int lua_body_center(lua_State* lua)
{
    Body* body = checkBody(lua, 1);

    lua_pushnumber(lua, body->x + body->width * 0.5f);
    lua_pushnumber(lua, body->y + body->height * 0.5f);

    return 2;
}

int lua_body_index(lua_State* lua)
{
    Body* body = checkBody(lua, 1);
    const char* key = luaL_checkstring(lua, 2);

    if (strcmp(key, "x") == 0)        { lua_pushnumber(lua, body->x); return 1; }
    if (strcmp(key, "y") == 0)        { lua_pushnumber(lua, body->y); return 1; }
    if (strcmp(key, "width") == 0)    { lua_pushnumber(lua, body->width); return 1; }
    if (strcmp(key, "height") == 0)   { lua_pushnumber(lua, body->height); return 1; }
    if (strcmp(key, "vx") == 0)       { lua_pushnumber(lua, body->velocityX); return 1; }
    if (strcmp(key, "vy") == 0)       { lua_pushnumber(lua, body->velocityY); return 1; }
    if (strcmp(key, "dynamic") == 0)  { lua_pushboolean(lua, body->dynamic); return 1; }
    if (strcmp(key, "solid") == 0)    { lua_pushboolean(lua, body->solid); return 1; }
    if (strcmp(key, "grounded") == 0) { lua_pushboolean(lua, body->grounded); return 1; }
    if (strcmp(key, "name") == 0)     { lua_pushstring(lua, body->name.c_str()); return 1; }

    luaL_getmetatable(lua, BODY_MT);
    lua_getfield(lua, -1, "__methods");
    lua_pushvalue(lua, 2);
    lua_rawget(lua, -2);

    return 1;
}

int lua_body_newindex(lua_State* lua)
{
    Body* body = checkBody(lua, 1);
    const char* key = luaL_checkstring(lua, 2);

    if (strcmp(key, "x") == 0)       { body->x = (float)luaL_checknumber(lua, 3); return 0; }
    if (strcmp(key, "y") == 0)       { body->y = (float)luaL_checknumber(lua, 3); return 0; }
    if (strcmp(key, "width") == 0)   { body->width = (float)luaL_checknumber(lua, 3); return 0; }
    if (strcmp(key, "height") == 0)  { body->height = (float)luaL_checknumber(lua, 3); return 0; }
    if (strcmp(key, "vx") == 0)      { body->velocityX = (float)luaL_checknumber(lua, 3); return 0; }
    if (strcmp(key, "vy") == 0)      { body->velocityY = (float)luaL_checknumber(lua, 3); return 0; }
    if (strcmp(key, "dynamic") == 0) { body->dynamic = lua_toboolean(lua, 3); return 0; }
    if (strcmp(key, "solid") == 0)   { body->solid = lua_toboolean(lua, 3); return 0; }

    return luaL_error(lua, "campo '%s' nao existe ou e somente leitura", key);
}

int lua_body_tostring(lua_State* lua)
{
    Body* body = checkBody(lua, 1);

    char buffer[192];

    snprintf(
        buffer, sizeof(buffer),
        "Body('%s' pos %.1f,%.1f size %.0fx%.0f)",
        body->name.c_str(),
        body->x, body->y, body->width, body->height
    );

    lua_pushstring(lua, buffer);

    return 1;
}

// -------------------------------------------------------------
//  world.*
// -------------------------------------------------------------

int lua_world_gravity(lua_State* lua)
{
    if (lua_isnoneornil(lua, 1))
    {
        lua_pushnumber(lua, g.gravity);
        return 1;
    }

    g.gravity = static_cast<float>(luaL_checknumber(lua, 1));

    return 0;
}

// -------------------------------------------------------------
//  engine.*
// -------------------------------------------------------------

int errorHandler(lua_State* lua)
{
    const char* message = lua_tostring(lua, 1);

    if (!message)
        message = "(erro sem mensagem)";

    luaL_traceback(lua, lua, message, 1);

    return 1;
}

bool callLuaHook(lua_State* lua, const char* name, double deltaTime, bool passDelta)
{
    int handlerIndex = lua_gettop(lua) + 1;

    lua_pushcfunction(lua, errorHandler);
    lua_getglobal(lua, name);

    if (!lua_isfunction(lua, -1))
    {
        lua_pop(lua, 2);
        return true;
    }

    int args = 0;

    if (passDelta)
    {
        lua_pushnumber(lua, deltaTime);
        args = 1;
    }

    if (lua_pcall(lua, args, 0, handlerIndex) != LUA_OK)
    {
        std::cerr << "[LuaSun] erro em " << name << "():\n"
                  << lua_tostring(lua, -1) << std::endl;

        lua_pop(lua, 2); // mensagem + handler
        return false;
    }

    lua_pop(lua, 1); // handler

    return true;
}

int lua_engine_run(lua_State* lua)
{
    if (g.window == nullptr || g.renderer == nullptr)
        return luaL_error(lua, "crie a janela com window.create() antes de engine.run()");

    if (g.insideLoop)
        return luaL_error(lua, "engine.run() ja esta em execucao");

    g.insideLoop = true;
    g.running = true;

    Uint64 previousTime = SDL_GetPerformanceCounter();
    const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());

    callLuaHook(lua, "start", 0.0, false);

    while (g.running)
    {
        Uint64 currentTime = SDL_GetPerformanceCounter();

        double deltaTime =
            static_cast<double>(currentTime - previousTime) / frequency;

        previousTime = currentTime;

        // trava o delta: sem isto, um travamento do sistema faz o
        // personagem atravessar paredes no frame seguinte
        if (deltaTime > 0.05) deltaTime = 0.05;
        if (deltaTime < 0.0)  deltaTime = 0.0;

        g.elapsed += deltaTime;
        g.frame++;

        if (deltaTime > 0.0)
            g.fps = g.fps * 0.92 + (1.0 / deltaTime) * 0.08;

        // ---- eventos ----
        SDL_Event event;

        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
                g.running = false;

            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE)
                g.running = false;
        }

        // ---- input ----
        memcpy(g.keyPrev, g.keyNow, sizeof(g.keyNow));

        const Uint8* state = SDL_GetKeyboardState(nullptr);
        memcpy(g.keyNow, state, sizeof(g.keyNow));

        g.mousePrev = g.mouseNow;
        g.mouseNow = SDL_GetMouseState(&g.mouseX, &g.mouseY);

        // ---- update ----
        if (!callLuaHook(lua, "update", deltaTime, true))
        {
            g.running = false;
            break;
        }

        updatePhysics(static_cast<float>(deltaTime));

        // ---- render ----
        SDL_SetRenderDrawColor(
            g.renderer,
            g.clearColor.r,
            g.clearColor.g,
            g.clearColor.b,
            255
        );

        SDL_RenderClear(g.renderer);

        if (!callLuaHook(lua, "render", 0.0, false))
        {
            g.running = false;
            break;
        }

        SDL_RenderPresent(g.renderer);

        if ((g.frame % 600) == 0)
            purgeTextCache();

        if (g.targetFps > 0)
        {
            double frameTime =
                static_cast<double>(SDL_GetPerformanceCounter() - currentTime)
                / frequency;

            double budget = 1.0 / static_cast<double>(g.targetFps);

            if (frameTime < budget)
                SDL_Delay(static_cast<Uint32>((budget - frameTime) * 1000.0));
        }
    }

    g.insideLoop = false;

    return 0;
}

int lua_engine_quit(lua_State* lua)
{
    (void)lua;
    g.running = false;
    return 0;
}

int lua_engine_fps(lua_State* lua)
{
    lua_pushnumber(lua, g.fps);
    return 1;
}

int lua_engine_time(lua_State* lua)
{
    lua_pushnumber(lua, g.elapsed);
    return 1;
}

int lua_engine_target_fps(lua_State* lua)
{
    g.targetFps = static_cast<int>(luaL_optinteger(lua, 1, 0));

    if (g.targetFps < 0)
        g.targetFps = 0;

    return 0;
}

// -------------------------------------------------------------
//  Registro da API
// -------------------------------------------------------------

void registerModule(lua_State* lua, const char* name, const luaL_Reg* functions)
{
    lua_newtable(lua);
    luaL_setfuncs(lua, functions, 0);
    lua_setglobal(lua, name);
}

void registerBodyMetatable(lua_State* lua)
{
    static const luaL_Reg methods[] =
    {
        { "draw",        lua_draw_body },
        { "collides",    lua_body_collides },
        { "destroy",     lua_body_destroy },
        { "move",        lua_body_move },
        { "setPosition", lua_body_set_position },
        { "setVelocity", lua_body_set_velocity },
        { "center",      lua_body_center },
        { nullptr, nullptr }
    };

    luaL_newmetatable(lua, BODY_MT);

    lua_pushcfunction(lua, lua_body_index);
    lua_setfield(lua, -2, "__index");

    lua_pushcfunction(lua, lua_body_newindex);
    lua_setfield(lua, -2, "__newindex");

    lua_pushcfunction(lua, lua_body_tostring);
    lua_setfield(lua, -2, "__tostring");

    lua_pushstring(lua, BODY_MT);
    lua_setfield(lua, -2, "__name");

    lua_newtable(lua);
    luaL_setfuncs(lua, methods, 0);
    lua_setfield(lua, -2, "__methods");

    lua_pop(lua, 1);
}

void registerLuaApi(lua_State* lua)
{
    registerBodyMetatable(lua);

    static const luaL_Reg windowApi[] =
    {
        { "create",     lua_window_create },
        { "title",      lua_window_title },
        { "size",       lua_window_size },
        { "background", lua_window_background },
        { nullptr, nullptr }
    };

    static const luaL_Reg engineApi[] =
    {
        { "run",       lua_engine_run },
        { "quit",      lua_engine_quit },
        { "fps",       lua_engine_fps },
        { "time",      lua_engine_time },
        { "targetFps", lua_engine_target_fps },
        { nullptr, nullptr }
    };

    static const luaL_Reg drawApi[] =
    {
        { "rectangle", lua_draw_rectangle },
        { "outline",   lua_draw_outline },
        { "circle",    lua_draw_circle },
        { "line",      lua_draw_line },
        { "image",     lua_draw_image },
        { "sprite",    lua_draw_sprite },
        { "text",      lua_draw_text },
        { "measure",   lua_draw_measure },
        { "body",      lua_draw_body },
        { "clearCache", lua_assets_clear },
        { nullptr, nullptr }
    };

    static const luaL_Reg inputApi[] =
    {
        { "key",           lua_input_key },
        { "pressed",       lua_input_pressed },
        { "released",      lua_input_released },
        { "mouse",         lua_input_mouse },
        { "mouseDown",     lua_input_mouse_down },
        { "mousePressed",  lua_input_mouse_pressed },
        { "mouseReleased", lua_input_mouse_released },
        { nullptr, nullptr }
    };

    static const luaL_Reg bodyApi[] =
    {
        { "dynamic",  lua_body_dynamic },
        { "static",   lua_body_static },
        { "get",      lua_body_get },
        { "collides", lua_body_collides },
        { "destroy",  lua_body_destroy },
        { nullptr, nullptr }
    };

    static const luaL_Reg worldApi[] =
    {
        { "gravity", lua_world_gravity },
        { nullptr, nullptr }
    };

    registerModule(lua, "window", windowApi);
    registerModule(lua, "engine", engineApi);
    registerModule(lua, "draw", drawApi);
    registerModule(lua, "input", inputApi);
    registerModule(lua, "body", bodyApi);
    registerModule(lua, "world", worldApi);
}

void shutdown()
{
    clearAssets();

    if (g.renderer)
    {
        SDL_DestroyRenderer(g.renderer);
        g.renderer = nullptr;
    }

    if (g.window)
    {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }

    if (g.sdlReady)
    {
        TTF_Quit();
        IMG_Quit();
        SDL_Quit();
        g.sdlReady = false;
    }
}

} // namespace

// -------------------------------------------------------------
//  main
// -------------------------------------------------------------

int main(int argc, char** argv)
{
    const char* scriptPath = (argc > 1) ? argv[1] : "game/main.lua";

    std::cout << "LuaSun iniciando (" << scriptPath << ")..." << std::endl;

    lua_State* lua = luaL_newstate();

    if (!lua)
    {
        std::cerr << "[LuaSun] falha ao criar o estado Lua." << std::endl;
        return 1;
    }

    luaL_openlibs(lua);
    registerLuaApi(lua);

    int status = 0;

    lua_pushcfunction(lua, errorHandler);

    int handlerIndex = lua_gettop(lua);

    if (luaL_loadfile(lua, scriptPath) != LUA_OK ||
        lua_pcall(lua, 0, 0, handlerIndex) != LUA_OK)
    {
        std::cerr << "[LuaSun] erro no script:\n"
                  << lua_tostring(lua, -1) << std::endl;

        status = 1;
    }

    // libera as texturas/fontes ANTES de destruir o renderer
    shutdown();

    lua_close(lua);

    std::cout << "LuaSun encerrado." << std::endl;

    return status;
}
