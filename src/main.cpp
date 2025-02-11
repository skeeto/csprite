#if defined(_WIN32) && defined(_MSC_VER)
	#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#include <string>
#include <chrono>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include <SDL.h>
#include <SDL_thread.h>

#include "pfd.h"
#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_extension.h"

#include "stb_image_write.h"
#include "stb_image.h"
#include "utils.h"

#include "log/log.h"
#include "assets.h"
#include "logger.h"
#include "macros.h"
#include "tools.h"
#include "config.h"
#include "theme.h"
#include "palette.h"
#include "history.h"
#include "system.h"
#include "renderer/canvas.h"
#include "renderer/renderer.h"

typedef unsigned char uchar_t;

int32_t WindowDims[2] = { 700, 500 };
uint32_t CanvasDims[2] = { 32, 24 };

uint32_t MousePos[2] = { 0, 0 };
uint32_t MousePosDown[2] = { 0, 0 };
uint32_t MousePosLast[2] = { 0, 0 };
uint32_t MousePosRel[2] = { 0, 0 };
uint32_t MousePosDownRel[2] = { 0, 0 };
uint32_t MousePosRelLast[2] = { 0, 0 };

bool ShouldClose = false;
bool IsLMBDown = false;
bool IsCtrlDown = false;
bool IsShiftDown = false;
bool ShouldSave = false;
bool ShouldSaveAs = false;
bool CanvasMutable = true; // If Canvas's Data Can Be Changed Or Not
bool CanvasLocked = false;  // Same As `CanvasMutable` but with conditions like if any window is being hover or not
bool CanvasDidMutate = false;
bool EventsLocked = true;
bool UpdateWindowTitle = false;

char FilePath[SYS_PATH_MAX_SIZE] = "untitled.png";
char FileName[SYS_PATH_MAX_SIZE] = "untitled.png";

#define MAX_CANVAS_LAYERS 100
uint32_t SelectedLayerIndex = 0;
CanvasLayer_T* CanvasLayers[MAX_CANVAS_LAYERS] = { NULL };
#define CURR_CANVAS_LAYER CanvasLayers[SelectedLayerIndex]

enum tool_e { BRUSH_COLOR, BRUSH_ERASER, SHAPE_LINE, SHAPE_RECT, SHAPE_CIRCLE, TOOL_INKDROPPER, TOOL_FLOODFILL, TOOL_PAN };
enum tool_shape_e { CIRCLE, SQUARE };
enum tool_e Tool = BRUSH_COLOR;
enum tool_e LastTool = BRUSH_COLOR;
enum tool_shape_e ToolShape = CIRCLE;

float PreviewWindowZoom = 1.0f;
float CurrViewportZoom = 1.0f;

// Using GLint & GLsizei as it's specified in the documentation and using float or something else causes glitches like viewport suddenly disappearing
GLint ViewportPos[2] = { 0, 0 };
GLsizei ViewportSize[2] = { 0, 0 };

int32_t PaletteIndex = 0;
int32_t ThemeIndex = 0;
uint16_t PaletteColorIndex = 2;
uchar_t EraseColor[4] = { 0, 0, 0, 0 };
uchar_t SelectedColor[4] = { 255, 255, 255, 255 };

#define MAX_SELECTEDTOOLTEXT_SIZE 512
char SelectedToolText[MAX_SELECTEDTOOLTEXT_SIZE] = "";

Config_T* AppConfig = NULL;
PaletteArr_T* PaletteArr = NULL;
theme_arr_t* ThemeArr = NULL;

#ifndef CS_VERSION_MAJOR
	#define CS_VERSION_MAJOR 0
#endif

#ifndef CS_VERSION_MAJOR
	#define CS_VERSION_MINOR 0
#endif

#ifndef CS_VERSION_MAJOR
	#define CS_VERSION_PATCH 0
#endif

#ifndef CS_BUILD_STABLE
	#define CS_BUILD_STABLE 0
#endif

#if CS_BUILD_STABLE == 0
	#define CS_BUILD_TYPE "dev"
#else
	#define CS_BUILD_TYPE "stable"
#endif

#define VERSION_STR "v" + std::to_string(CS_VERSION_MAJOR) + \
						"." + std::to_string(CS_VERSION_MINOR) + \
						"." + std::to_string(CS_VERSION_PATCH) + \
						"-" + CS_BUILD_TYPE

#define WINDOW_TITLE_CSTR ( \
	FileName[0] != 0 ? \
		FileName + std::string(" - csprite ") + VERSION_STR : \
		std::string("csprite - ") + VERSION_STR \
	).c_str()

static inline void ProcessEvents(SDL_Window* window);
static void _GuiSetColors(ImGuiStyle& style);
static void _GuiSetToolText();
void ZoomOpenGlViewport(int increase);
void fill(uint32_t x, uint32_t y, unsigned char *old_colour);
void UpdateViewportSize();
void UpdateViewportPos();
static inline bool CanMutateCanvas();
void MutateCanvas(bool LmbJustReleased);
uchar_t* GetPixel(int x, int y);
static void InitWindowIcon(SDL_Window* window);
static void OpenNewFile(SDL_Window* window);

#define GetSelectedPalette() PaletteArr->Palettes[PaletteIndex]

#define UNDO() \
	if (CURR_CANVAS_LAYER != NULL) HISTORY_UNDO(CURR_CANVAS_LAYER->history, CanvasDims[0] * CanvasDims[1] * 4 * sizeof(uchar_t), CURR_CANVAS_LAYER->pixels)

#define REDO() \
	if (CURR_CANVAS_LAYER != NULL) HISTORY_REDO(CURR_CANVAS_LAYER->history, CanvasDims[0] * CanvasDims[1] * 4 * sizeof(uchar_t), CURR_CANVAS_LAYER->pixels)

struct sdl_thread_arg {
	SDL_Window* win;
	int argc;
	char** argv;
};

/*
	This Function Handles All The Rendering Stuff,
	Changing Anything Of Window Like Title And Stuff
	Should Be Done In THe Event Thread Where The Window Was Initialized.
	void* args = struct sdl_thread_arg* args;
*/
int RendererThreadFunc(void* _args) {
	if (_args == NULL) return -1;
	struct sdl_thread_arg* args = (struct sdl_thread_arg*)_args;
	int argc = args->argc;
	char** argv = args->argv;
	SDL_Window* window = args->win;
	if (window == NULL) return -1;

	if (R_Init(window, AppConfig->vsync) != EXIT_SUCCESS) {
		return EXIT_FAILURE;
	}

	UpdateViewportPos();
	UpdateViewportSize();

	if (Canvas_Init(CanvasDims[0], CanvasDims[1]) != EXIT_SUCCESS) {
		return EXIT_FAILURE;
	}

	CURR_CANVAS_LAYER = Canvas_CreateLayer();
	if (CURR_CANVAS_LAYER == NULL) return EXIT_FAILURE;

	if (argc > 1) {
		const char* filePath = (const char*)argv[1];
		int result = Sys_IsRegularFile(filePath);
		if (result < 0) {
			Logger_Error("Error Trying To Valid File Path: %s", strerror(errno));
		} else if (result == 0) {
			Logger_Error("Cannot Open The File in filePath");
		} else {
			int w = 0, h = 0, channels = 0;
			uchar_t* _data = stbi_load(filePath, &w, &h, &channels, 4);
			if (w > 0 && h > 0) {
				for (uint32_t i = 0; i < MAX_CANVAS_LAYERS; ++i) {
					if (CanvasLayers[i] != NULL) {
						Canvas_DestroyLayer(CanvasLayers[i]);
						CanvasLayers[i] = NULL;
					}
				}
				if ((uint32_t)w != CanvasDims[0] || (uint32_t)h != CanvasDims[1]) { // If The Image We Are Opening Doesn't Has Same Resolution As Our Current Image Then Resize The Canvas
					Canvas_Resize(w, h);
					CanvasDims[0] = w;
					CanvasDims[1] = h;
					CurrViewportZoom = 1.0f;
					UpdateViewportSize();
					UpdateViewportPos();
				}

				SelectedLayerIndex = 0;
				CURR_CANVAS_LAYER = Canvas_CreateLayer();
				memcpy(CURR_CANVAS_LAYER->pixels, _data, w * h * 4 * sizeof(uchar_t));
				FreeHistory(&CURR_CANVAS_LAYER->history);
				SaveHistory(&CURR_CANVAS_LAYER->history, w * h * 4 * sizeof(uchar_t), CURR_CANVAS_LAYER->pixels);

				snprintf(FilePath, SYS_PATH_MAX_SIZE, "%s", filePath);
				char* filePathBasename = Sys_GetBasename(filePath);
				snprintf(FileName, SYS_PATH_MAX_SIZE, "%s", filePathBasename);
				free(filePathBasename);
				stbi_image_free(_data);
				UpdateWindowTitle = true;
			}
		}
	}

	ImGuiIO& io = ImGui::GetIO();
	ImGuiStyle& style = ImGui::GetStyle();
	io.IniFilename = nullptr;
	io.LogFilename = nullptr;
	ImGui::StyleColorsDark();
	ThemeArr = ThemeLoadAll();
	_GuiSetColors(style);

	{
		int defaultUiFontSize = 0;
		const void* defaultUiFont = Assets_Get("data/fonts/bm-mini.ttf", &defaultUiFontSize);
		if (defaultUiFont) io.Fonts->AddFontFromMemoryCompressedTTF(defaultUiFont, defaultUiFontSize, 16.0f);
	}

	SelectedColor[0] = GetSelectedPalette()->Colors[PaletteColorIndex][0];
	SelectedColor[1] = GetSelectedPalette()->Colors[PaletteColorIndex][1];
	SelectedColor[2] = GetSelectedPalette()->Colors[PaletteColorIndex][2];
	SelectedColor[3] = GetSelectedPalette()->Colors[PaletteColorIndex][3];

	bool ShowPreferencesWindow = false;
	bool ShowLayerRenameWindow = false;
	bool ShowNewCanvasWindow = false;

	Logger_Hide();
	EventsLocked = false;

	unsigned int frameStart, frameTime;
	const unsigned int frameDelay = 1000 / AppConfig->FramesUpdateRate;

	while (!ShouldClose) {
		frameStart = SDL_GetTicks();
		CanvasLocked = !CanvasMutable || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || ShowLayerRenameWindow || ShowPreferencesWindow || ShowNewCanvasWindow;

		R_Clear(); // Clear The Screen, This Is Required To Done Before The Canvas Is Drawn Because Rendered Canvas Is Directly Copied Onto Screen & Clearing The screen After Copying It Will Not Show The Canvas
		R_NewFrame(); // All The Calls To ImGui Will Be Recorded After This Function

		if (ImGui::BeginMainMenuBar()) {
			if (ImGui::BeginMenu("File")) {
				if (ImGui::MenuItem("New")) {
					ShowNewCanvasWindow = true;
				}
				if (ImGui::MenuItem("Open", "Ctrl+O")) {
					OpenNewFile(window);
				}
				if (ImGui::BeginMenu("Save")) {
					if (ImGui::MenuItem("Save", "Ctrl+S")) {
						ShouldSave = true;
					}
					if (ImGui::MenuItem("Save As", "Ctrl+Shift+S")) {
						ShouldSaveAs = true;
					}
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit")) {
				if (ImGui::MenuItem("Undo", "Ctrl+Z", false, (CURR_CANVAS_LAYER != NULL && CURR_CANVAS_LAYER->history->prev != NULL))) {
					UNDO();
				}
				if (ImGui::MenuItem("Redo", "Ctrl+Y", false, (CURR_CANVAS_LAYER != NULL && CURR_CANVAS_LAYER->history->next != NULL))) {
					REDO();
				}
				if (ImGui::BeginMenu("Palette")) {
					for (int32_t i = 0; i < PaletteArr->numOfEntries; ++i) {
						int32_t _palidx = PaletteIndex;

						if (ImGui::MenuItem(PaletteArr->Palettes[i]->name, NULL)) {
							PaletteIndex = i;
						}
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
							ImGui::SetTooltip("%s", PaletteArr->Palettes[i]->author);
						}
						if (_palidx == i) {
							ImGui::SameLine();
							ImGui::Text("<");
						}
					}
					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Theme")) {
					for (int32_t i = 0; i < ThemeArr->numOfEntries; ++i) {
						int32_t _themeidx = ThemeIndex;

						if (ImGui::MenuItem(ThemeArr->entries[i]->name, NULL)) {
							ThemeIndex = i;
							_GuiSetColors(style);
						}
						if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
							ImGui::SetTooltip("%s", ThemeArr->entries[i]->author);
						}

						if (_themeidx == i) {
							ImGui::SameLine();
							ImGui::Text("<");
						}
					}
					ImGui::EndMenu();
				}

				if (ImGui::MenuItem("Preferences")) {
					ShowPreferencesWindow = true;
				}

				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("View")) {
				if (ImGui::MenuItem("Logs")) {
					if (Logger_IsHidden()) Logger_Show();
					else Logger_Hide();
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Help")) {
				if (ImGui::MenuItem("Wiki")) {
					OpenURL("https://csprite.github.io/wiki/");
				}
				if (ImGui::MenuItem("About")) {
					OpenURL("https://github.com/pegvin/csprite/wiki/About-CSprite");
				}
				if (ImGui::MenuItem("GitHub")) {
					OpenURL("https://github.com/pegvin/csprite");
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}

		ImVec2 PalWinSize;
		if (ImGui::Begin("Palettes", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse)) {
			ImGui::SetWindowPos({ 3.0f, 30.0f });
			for (unsigned int i = 0; i < GetSelectedPalette()->numOfEntries; i++) {
				if (i != 0 && i % 2 != 0) ImGui::SameLine();
				if (ImGui::ColorButton(PaletteColorIndex == i ? "Selected Color" : ("Color##" + std::to_string(i)).c_str(), {
					((float)(GetSelectedPalette()->Colors[i][0]) / 255),
					((float)(GetSelectedPalette()->Colors[i][1]) / 255),
					((float)(GetSelectedPalette()->Colors[i][2]) / 255),
					((float)(GetSelectedPalette()->Colors[i][3]) / 255)
				})) {
					PaletteColorIndex = i;
					SelectedColor[0] = GetSelectedPalette()->Colors[PaletteColorIndex][0];
					SelectedColor[1] = GetSelectedPalette()->Colors[PaletteColorIndex][1];
					SelectedColor[2] = GetSelectedPalette()->Colors[PaletteColorIndex][2];
					SelectedColor[3] = GetSelectedPalette()->Colors[PaletteColorIndex][3];
				}

				ImGui::GetWindowDrawList()->AddRect(
					ImGui::GetItemRectMin(),
					ImGui::GetItemRectMax(),
					(PaletteColorIndex == i && COLOR_EQUAL(SelectedColor, GetSelectedPalette()->Colors[i])) ? 0xFFFFFFFF : 0x000000FF,
					0, 0, 1
				);
			};

			ImGui::SetWindowSize({0.0f, 0.0f}); // This Will Make the window adjust size according to children
			PalWinSize = ImGui::GetWindowSize();
			ImGui::End();
		}

		if (ImGui::Begin("###ColorPickerWindow", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground)) {
			ImGui::SetWindowPos({ 0, PalWinSize.y + 40.0f });
			float ImColPicker[4] = { (float)(SelectedColor[0]) / 255, (float)(SelectedColor[1]) / 255, (float)(SelectedColor[2]) / 255, (float)(SelectedColor[3]) / 255 };
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 8));
			ImGui::ColorEdit4(
				"##ColorPickerWidget", (float*)&ImColPicker,
				ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel
			);
			ImGui::PopStyleVar();

			SelectedColor[0] = ImColPicker[0] * 255;
			SelectedColor[1] = ImColPicker[1] * 255;
			SelectedColor[2] = ImColPicker[2] * 255;
			SelectedColor[3] = ImColPicker[3] * 255;

			ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), 0xFFFFFFFF, 0, 0, 1);
			ImGui::SetWindowSize({ 0, 0 });
			ImGui::End();
		}

		if (ImGui::Begin(
			"Debug Window", NULL,
			ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize
#if(CS_BUILD_STABLE == 1)
			| ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground
#endif
		)) {
			ImVec2 WinSize = ImGui::GetWindowSize();
			ImGui::SetWindowPos({ 5, io.DisplaySize.y - WinSize.y - 10 });
			ImGui::Text("%s\nZoom: %d%%", SelectedToolText, (int)(CurrViewportZoom * 100));
#if(CS_BUILD_STABLE == 0)
			ImGui::Text("Average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
			if (ImGui::Button("Clear Undo/Redo Buffers")) {
				for (int i = 0; i < MAX_CANVAS_LAYERS; ++i) {
					if (CanvasLayers[i] != NULL) {
						FreeHistory(&CanvasLayers[i]->history);
						SaveHistory(&CanvasLayers[i]->history, CanvasDims[0] * CanvasDims[1] * 4 * sizeof(uchar_t), CanvasLayers[i]->pixels);
					}
				}
			}
#endif
			ImGui::End();
		}

		static bool ResetPreviewWindowSize = true;
		static bool ResetPreviewWindowPos = true;

		if (ImGui::Begin("Preview", NULL, ImGuiWindowFlags_NoCollapse)) {
			if (ImGui::BeginPopupContextItem()) {
				if (ImGui::MenuItem("Reset Size")) {
					ResetPreviewWindowSize = true;
				}
				if (ImGui::MenuItem("Reset Position")) {
					ResetPreviewWindowPos = true;
				}
				ImGui::EndPopup();
			}
			static ImVec2 WinSize = { 192.0f, 168.0f };
			// XX - Use "SetNextWindowPos" & "SetNextWindowSize" If The Window Is Movable Or Else The Children Won't Be Able To Move
			if (ResetPreviewWindowPos == true) {
				ImGui::SetWindowPos({ io.DisplaySize.x - WinSize.x - 20, io.DisplaySize.y - WinSize.y - 20, }); // Move Window To Bottom Right With 20 pixel padding
				ResetPreviewWindowPos = false;
			}
			if (ResetPreviewWindowSize == true) {
				ImGui::SetWindowSize({ 192.0f, 168.0f });
				ResetPreviewWindowSize = false;
			}
			ImGui::Image(
				reinterpret_cast<ImTextureID>(Canvas_GetFBOTex()), // FBO Texture
				{ WinSize.x - 15, CanvasDims[1] * (WinSize.x - 15) / CanvasDims[0] },
				ImVec2(0,1), ImVec2(1,0) // UV
			);
			WinSize = ImGui::GetWindowSize();
			ImGui::End();
		}

		if (ImGui::Begin("Layer", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
			ImVec2 WinSize = { 200, 300 };
			ImGui::SetWindowPos({ io.DisplaySize.x - WinSize.x - 10, 40, }); // Move Window To Bottom Right With 20 pixel padding
			ImGui::SetWindowSize(WinSize, ImGuiCond_FirstUseEver);

			if (ImGui::Button("+")) {
				if (SelectedLayerIndex + 1 != MAX_CANVAS_LAYERS) {
					if (CURR_CANVAS_LAYER == NULL) {
						CURR_CANVAS_LAYER = Canvas_CreateLayer();
					} else {
						if (SelectedLayerIndex + 1 < MAX_CANVAS_LAYERS) {
IncrementAndCreateLayer__:
							SelectedLayerIndex++;
							if (SelectedLayerIndex + 1 != MAX_CANVAS_LAYERS) {
								if (CURR_CANVAS_LAYER != NULL) goto IncrementAndCreateLayer__;
							}
							CURR_CANVAS_LAYER = Canvas_CreateLayer();
						}
					}
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("-")) {
				if (CURR_CANVAS_LAYER != NULL) {
					Canvas_DestroyLayer(CURR_CANVAS_LAYER);
					CURR_CANVAS_LAYER = NULL;
					if (SelectedLayerIndex >= 1) SelectedLayerIndex--;
					else SelectedLayerIndex++;
				}
			}

			for (uint32_t i = 0; i < MAX_CANVAS_LAYERS; ++i) {
				if (CanvasLayers[i] != NULL) {
					if (ImGui::Selectable(CanvasLayers[i]->name, SelectedLayerIndex == i, ImGuiSelectableFlags_AllowDoubleClick)) {
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
							ShowLayerRenameWindow = true;
						} else {
							SelectedLayerIndex = i;
						}
					}
				}
			}
			ImGui::End();
		}

		if (ShowNewCanvasWindow) {
			if (ImGui::BeginPopupModal("Create New###NewCanvasWindow", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
				static int32_t NewDims[2] = { 32, 24 };
				ImGui::InputInt("Width", &NewDims[0], 1, 5);
				ImGui::InputInt("Height", &NewDims[1], 1, 5);

				if (ImGui::Button("Create")) {
					if (NewDims[0] > 0 && NewDims[1] > 0) {
						for (uint32_t i = 0; i < MAX_CANVAS_LAYERS; ++i) {
							if (CanvasLayers[i] != NULL) {
								Canvas_DestroyLayer(CanvasLayers[i]);
								CanvasLayers[i] = NULL;
							}
						}
						SelectedLayerIndex = 0;
						Canvas_Resize(NewDims[0], NewDims[1]);
						CURR_CANVAS_LAYER = Canvas_CreateLayer();
						CanvasDims[0] = NewDims[0];
						CanvasDims[1] = NewDims[1];
						CurrViewportZoom = 1.0f;
						UpdateViewportSize();
						UpdateViewportPos();
					}
					ShowNewCanvasWindow = false;
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) {
					NewDims[0] = 32;
					NewDims[1] = 24;
					ShowNewCanvasWindow = false;
				}
				ImGui::EndPopup();
			} else {
				ImGui::OpenPopup("Create New###NewCanvasWindow");
			}
		}

		if (ShowPreferencesWindow) {
			if (ImGui::BeginPopupModal("Preferences###PreferencesWindow", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::Text("VSync (%s)", AppConfig->vsync ? "Enabled" : "Disabled");
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Vertical Sync - Synchronize App's FPS To Your Monitor's FPS");
				ImGui::SameLine();
				ImGui::Ext_ToggleButton("VSync_Toggle", &AppConfig->vsync);

				ImGui::InputInt("FPS", &AppConfig->FramesUpdateRate, 1, 5, AppConfig->vsync == true ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Frames Per Second");

				ImGui::InputInt("EPS", &AppConfig->EventsUpdateRate, 1, 5);
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Events Per Second");

				AppConfig->EventsUpdateRate = AppConfig->EventsUpdateRate < 5 ? 5 : AppConfig->EventsUpdateRate;
				AppConfig->FramesUpdateRate = AppConfig->FramesUpdateRate < 5 ? 5 : AppConfig->FramesUpdateRate;
				if (ImGui::Button("Save")) {
					WriteConfig(AppConfig);
					ShowPreferencesWindow = false;
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("please restart the app after saving");
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) {
					ShowPreferencesWindow = false;
				}
				ImGui::EndPopup();
			} else {
				ImGui::OpenPopup("Preferences###PreferencesWindow");
			}
		}

		if (ShowLayerRenameWindow) {
			if (ImGui::BeginPopupModal("Rename Layer###LayerRenameWindow", NULL, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
				static char TempBuff[LAYER_NAME_MAX] = "";

				if (ImGui::InputText("##NewLayerName", TempBuff, LAYER_NAME_MAX, ImGuiInputTextFlags_EnterReturnsTrue)) {
					strncpy(CURR_CANVAS_LAYER->name, TempBuff, LAYER_NAME_MAX);
					memset(TempBuff, 0, LAYER_NAME_MAX);
					ShowLayerRenameWindow = false;
				}

				if (ImGui::Button("Ok")) {
					strncpy(CURR_CANVAS_LAYER->name, TempBuff, LAYER_NAME_MAX);
					memset(TempBuff, 0, LAYER_NAME_MAX);
					ShowLayerRenameWindow = false;
				}
				ImGui::SameLine();
				if (ImGui::Button("Cancel")) {
					ShowLayerRenameWindow = false;
				}
				ImGui::EndPopup();
			} else {
				ImGui::OpenPopup("Rename Layer###LayerRenameWindow");
			}
		}

		Logger_Draw("Logs");

		if (CURR_CANVAS_LAYER != NULL) {
			Canvas_NewFrame(!ShouldSave && !ShouldSaveAs);
			for (uint32_t i = 0; i < MAX_CANVAS_LAYERS; ++i) {
				if (CanvasLayers[i] != NULL) {
					Canvas_Layer(CanvasLayers[i], SelectedLayerIndex == i);
				}
			}
			Canvas_Render(ViewportPos[0], ViewportPos[1], ViewportSize[0], ViewportSize[1]); // This Is When Canvas Is Rendered To Screen

			if (ShouldSave == true || ShouldSaveAs == true) {
				if (ShouldSaveAs == true) {
					auto destination = pfd::save_file("Select a file", ".", { "Image Files", "*.png" }, pfd::opt::none).result();
					const char* _fPath = destination.empty() ? NULL : destination.c_str();
					if (_fPath != NULL) {
						snprintf(FilePath, SYS_PATH_MAX_SIZE, "%s", _fPath);
						char* _fName = Sys_GetBasename(_fPath);
						snprintf(FileName, SYS_PATH_MAX_SIZE, "%s", _fName);
						free(_fName);
						UpdateWindowTitle = true;
					} else {
						ShouldSaveAs = false;
					}
				}

				// ShouldSave or ShouldSaveAs Might Be Set To False If There Was An Error So We Need To Check It
				if (ShouldSave == true || ShouldSaveAs == true) {
					uchar_t* canvas_data = Canvas_GetRender();
					stbi_flip_vertically_on_write(1); // Flip Vertically Because Of OpenGL's Coordinate System
					stbi_write_png(FilePath, CanvasDims[0], CanvasDims[1], 4, canvas_data, 0);
					free(canvas_data);
					ShouldSave = false;
					ShouldSaveAs = false;
				}
			}
		}

		R_Present();

		if (!AppConfig->vsync) {
			frameTime = SDL_GetTicks() - frameStart;
			if (frameDelay > frameTime) SDL_Delay(frameDelay - frameTime);
			frameStart = SDL_GetTicks();
		}
	}

	Canvas_Destroy();

	for (int i = 0; i < MAX_CANVAS_LAYERS; ++i) {
		if (CanvasLayers[i] != NULL) {
			Canvas_DestroyLayer(CanvasLayers[i]);
			CanvasLayers[i] = NULL;
		}
	}

	R_Destroy();
	return 0;
}

int main(int argc, char* argv[]) {
	FILE* LogFilePtr = fopen("csprite.log", "w");
	log_add_fp(LogFilePtr, LOG_TRACE);

	AppConfig = LoadConfig();
	PaletteArr = PaletteLoadAll();

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
		Logger_Error("failed to initialize SDL2: %s", SDL_GetError());
		return EXIT_FAILURE;
	}

	SDL_version compiled;
	SDL_version linked;
	SDL_VERSION(&compiled);
	SDL_GetVersion(&linked);

	Logger_Info("Compiled With SDL version %u.%u.%u", compiled.major, compiled.minor, compiled.patch);
	Logger_Info("Linked With SDL version %u.%u.%u", linked.major, linked.minor, linked.patch);

	SDL_DisplayMode dm;
	SDL_GetCurrentDisplayMode(0, &dm);
	WindowDims[0] = dm.w * 0.7;
	WindowDims[1] = dm.h * 0.8;

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	SDL_Window* window = SDL_CreateWindow(
		WINDOW_TITLE_CSTR,
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		WindowDims[0], WindowDims[1],
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL | SDL_WINDOW_MAXIMIZED
	);
	SDL_GetWindowSize(window, &WindowDims[0], &WindowDims[1]);
	UpdateViewportPos();

	InitWindowIcon(window);
	SDL_ShowWindow(window);
	_GuiSetToolText();

	struct sdl_thread_arg* renderer_args = (struct sdl_thread_arg*)malloc(sizeof(struct sdl_thread_arg));
	renderer_args->argc = argc;
	renderer_args->argv = argv;
	renderer_args->win = window;
	SDL_Thread* _RenderThread = SDL_CreateThread(RendererThreadFunc, "Renderer Thread", renderer_args);

	unsigned int frameStart, frameTime;
	const unsigned int frameDelay = 1000 / AppConfig->EventsUpdateRate;
	while (!ShouldClose) {
		frameStart = SDL_GetTicks();
		if (!EventsLocked) {
			ProcessEvents(window);
			if (UpdateWindowTitle) {
				SDL_SetWindowTitle(window, WINDOW_TITLE_CSTR);
				UpdateWindowTitle = false;
			}
		}
		frameTime = SDL_GetTicks() - frameStart;
		if (frameDelay > frameTime) SDL_Delay(frameDelay - frameTime);
		frameStart = SDL_GetTicks();
	}

	SDL_WaitThread(_RenderThread, NULL);
	SDL_DestroyWindow(window);
	SDL_Quit();

	FreePaletteArr(PaletteArr);
	PaletteArr = NULL;

	free(renderer_args);
	renderer_args = NULL;
	return EXIT_SUCCESS;
}

static void _GuiSetToolText() {
	switch (Tool) {
	case BRUSH_COLOR:
	case BRUSH_ERASER:
		snprintf(SelectedToolText, MAX_SELECTEDTOOLTEXT_SIZE, "%s %s (Size: %d)", ToolShape == SQUARE ? "Square" : "Circle", Tool == BRUSH_COLOR ? "Brush" : "Eraser", Tools_GetBrushSize());
		break;
	case SHAPE_RECT:
	case SHAPE_LINE:
		snprintf(SelectedToolText, MAX_SELECTEDTOOLTEXT_SIZE, "%s %s (Width: %d)", ToolShape == SQUARE ? "Square" : "Rounded", Tool == SHAPE_LINE ? "Line" : "Rectangle", Tools_GetBrushSize());
		break;
	case SHAPE_CIRCLE:
		snprintf(SelectedToolText, MAX_SELECTEDTOOLTEXT_SIZE, "Circle (Boundary Width: %d)", Tools_GetBrushSize());
		break;
	case TOOL_FLOODFILL:
	case TOOL_PAN:
		snprintf(SelectedToolText, MAX_SELECTEDTOOLTEXT_SIZE, "%s", Tool == TOOL_FLOODFILL ? "Flood Fill" : "Panning");
		break;
	case TOOL_INKDROPPER:
		snprintf(SelectedToolText, MAX_SELECTEDTOOLTEXT_SIZE, "Ink Dropper");
		break;
	}
}

static void _GuiSetColors(ImGuiStyle& style) {
	if (ThemeArr != NULL && ThemeArr->entries[ThemeIndex] != NULL) {
		theme_t* t = ThemeArr->entries[ThemeIndex];
		style = t->style;
	}
}

// Basically Checks if Canvas Is Not Locked, Current Layer isn't NULL, Mouse Positons Are In Bound
static inline bool CanMutateCanvas() {
	return (
		!CanvasLocked                   &&
		CURR_CANVAS_LAYER != NULL       &&
		MousePosRel[0] >= 0             &&
		MousePosRel[0] < CanvasDims[0]  &&
		MousePosRel[1] >= 0             &&
		MousePosRel[1] < CanvasDims[1]
	);
}

// Drawing And Stuff Is Done Here
void MutateCanvas(bool LmbJustReleased) {
	if (CanMutateCanvas() && (LmbJustReleased || IsLMBDown)) {
		switch (Tool) {
			case BRUSH_COLOR:
			case BRUSH_ERASER: {
				bool didChange = Tool_Brush(CURR_CANVAS_LAYER->pixels, Tool == BRUSH_COLOR ? SelectedColor : EraseColor, ToolShape == CIRCLE, MousePosRel[0], MousePosRel[1], CanvasDims[0], CanvasDims[1]);
				CanvasDidMutate = CanvasDidMutate || didChange;
				break;
			}
			case SHAPE_RECT:
			case SHAPE_CIRCLE:
			case SHAPE_LINE: {
				if (LmbJustReleased) {
					CanvasDidMutate = CanvasDidMutate || (Tool == SHAPE_LINE || Tool == SHAPE_RECT || Tool == SHAPE_CIRCLE);
					if (CanvasDidMutate == true) {
						SaveHistory(&CURR_CANVAS_LAYER->history, CanvasDims[0] * CanvasDims[1] * 4 * sizeof(uchar_t), CURR_CANVAS_LAYER->pixels);
						CanvasDidMutate = false;
					}
				} else if (IsLMBDown) {
					if (CURR_CANVAS_LAYER->history->prev != NULL) {
						memcpy(CURR_CANVAS_LAYER->pixels, CURR_CANVAS_LAYER->history->pixels, CanvasDims[0] * CanvasDims[1] * 4 * sizeof(uchar_t));
					} else {
						memset(CURR_CANVAS_LAYER->pixels, 0, CanvasDims[0] * CanvasDims[1] * 4 * sizeof(uchar_t));
					}
					if (Tool == SHAPE_RECT) {
						Tool_Rect(CURR_CANVAS_LAYER->pixels, SelectedColor, ToolShape == CIRCLE, MousePosDownRel[0], MousePosDownRel[1], MousePosRel[0], MousePosRel[1], CanvasDims[0], CanvasDims[1]);
					} else if (Tool == SHAPE_LINE) {
						Tool_Line(CURR_CANVAS_LAYER->pixels, SelectedColor, ToolShape == CIRCLE, MousePosDownRel[0], MousePosDownRel[1], MousePosRel[0], MousePosRel[1], CanvasDims[0], CanvasDims[1]);
					} else if (Tool == SHAPE_CIRCLE) {
						Tool_Circle(
							CURR_CANVAS_LAYER->pixels,
							SelectedColor,
							MousePosDownRel[0], MousePosDownRel[1],
							(int)sqrt( // Calculates Distance Between 2 x, y points
								(MousePosRel[0] - MousePosDownRel[0]) * (MousePosRel[0] - MousePosDownRel[0]) +
								(MousePosRel[1] - MousePosDownRel[1]) * (MousePosRel[1] - MousePosDownRel[1])
							),
							CanvasDims[0], CanvasDims[1]
						);
					}
				}
				break;
			}
			case TOOL_PAN: {
				break;
			}
			case TOOL_FLOODFILL: {
				if (LmbJustReleased) {
					unsigned char* pixel = GetCharData(CURR_CANVAS_LAYER->pixels, MousePosRel[0], MousePosRel[1], CanvasDims[0], CanvasDims[1]);
					unsigned char OldColor[4] = { *(pixel + 0), *(pixel + 1), *(pixel + 2), *(pixel + 3) };
					CanvasDidMutate = CanvasDidMutate || Tool_FloodFill(
						CURR_CANVAS_LAYER->pixels,
						OldColor, SelectedColor,
						MousePosRel[0], MousePosRel[1],
						CanvasDims[0], CanvasDims[1]
					);
				}
				break;
			}
			case TOOL_INKDROPPER: {
				if (LmbJustReleased) {
					uchar_t* pixel = GetPixel(MousePosRel[0], MousePosRel[1]);
					if (pixel != NULL && *(pixel + 3) != 0) {
						bool foundEntry = false;
						for (unsigned int i = 0; i < GetSelectedPalette()->numOfEntries; i++) {
							if (COLOR_EQUAL(GetSelectedPalette()->Colors[i], pixel)) {
								PaletteColorIndex = i;
								SelectedColor[0] = GetSelectedPalette()->Colors[PaletteColorIndex][0];
								SelectedColor[1] = GetSelectedPalette()->Colors[PaletteColorIndex][1];
								SelectedColor[2] = GetSelectedPalette()->Colors[PaletteColorIndex][2];
								SelectedColor[3] = GetSelectedPalette()->Colors[PaletteColorIndex][3];
								foundEntry = true;
								break;
							}
						}
						if (!foundEntry) {
							SelectedColor[0] = *(pixel + 0);
							SelectedColor[1] = *(pixel + 1);
							SelectedColor[2] = *(pixel + 2);
							SelectedColor[3] = *(pixel + 3);
						}
						Tool = LastTool;
						_GuiSetToolText();
					}
				}
				break;
			}
		}
	}
}

static inline void OnEvent_KeyUp(SDL_Event* e) {
	switch(e->key.keysym.sym) {
		case SDLK_SPACE:
			Tool = LastTool;
			CanvasMutable = true;
			_GuiSetToolText();
			break;
		case SDLK_f:
			Tool = TOOL_FLOODFILL;
			_GuiSetToolText();
			break;
		case SDLK_e:
			Tool = BRUSH_ERASER;
			ToolShape = IsShiftDown ? SQUARE : CIRCLE;
			_GuiSetToolText();
			break;
		case SDLK_b:
			Tool = BRUSH_COLOR;
			ToolShape = IsShiftDown ? SQUARE : CIRCLE;
			_GuiSetToolText();
			break;
		case SDLK_l:
			Tool = SHAPE_LINE;
			ToolShape = IsShiftDown ? SQUARE : CIRCLE;
			_GuiSetToolText();
			break;
		case SDLK_r:
			Tool = SHAPE_RECT;
			ToolShape = IsShiftDown ? SQUARE : CIRCLE;
			_GuiSetToolText();
			break;
		case SDLK_c:
			Tool = SHAPE_CIRCLE;
			_GuiSetToolText();
			break;
		case SDLK_s:
			if (IsCtrlDown == true && ShouldSave == false) ShouldSave = true;
			break;
		case SDLK_o:
			if (IsCtrlDown == true) OpenNewFile(SDL_GetWindowFromID(e->window.windowID));
			break;
		case SDLK_LCTRL:
		case SDLK_RCTRL:
			IsCtrlDown = false;
			break;
		case SDLK_LSHIFT:
		case SDLK_RSHIFT:
			IsShiftDown = false;
			break;
	}
}

static inline void OnEvent_KeyDown(SDL_Event* e) {
	switch(e->key.keysym.sym) {
		case SDLK_SPACE:
			if (Tool != TOOL_PAN) {
				LastTool = Tool;
				Tool = TOOL_PAN;
				CanvasMutable = false;
				_GuiSetToolText();
			}
			break;
		case SDLK_LCTRL:
		case SDLK_RCTRL:
			IsCtrlDown = true;
			break;
		case SDLK_LSHIFT:
		case SDLK_RSHIFT:
			IsShiftDown = true;
			break;
		case SDLK_z:
			if (IsCtrlDown && CURR_CANVAS_LAYER != NULL) {
				UNDO();
			}
			break;
		case SDLK_y:
			if (IsCtrlDown && CURR_CANVAS_LAYER != NULL) {
				REDO();
			}
			break;
		case SDLK_i:
			if (Tool != TOOL_INKDROPPER) {
				LastTool = Tool;
				Tool = TOOL_INKDROPPER;
				_GuiSetToolText();
			}
			break;
	}
}

static inline void OnEvent_MouseWheel(SDL_Event* e) {
	if (IsCtrlDown == true) {
		if (e->wheel.y > 0) { // Scroll Up - Zoom In
			ZoomOpenGlViewport(1);
		} else if (e->wheel.y < 0) { // Scroll Down - Zoom Out
			ZoomOpenGlViewport(0);
		}
	} else {
		uint32_t BrushSize = Tools_GetBrushSize();
		if (e->wheel.y > 0) {
			BrushSize++;
		} else if (e->wheel.y < 0) {
			if (BrushSize > 1) {
				BrushSize--;
			}
		}
		Tools_SetBrushSize(BrushSize);
		_GuiSetToolText();
	}
}

static inline void OnEvent_MouseMotion(SDL_Event* e) {
	MousePosLast[0] = MousePos[0];
	MousePosLast[1] = MousePos[1];
	MousePos[0] = e->motion.x;
	MousePos[1] = e->motion.y;

	MousePosRelLast[0] = MousePosRel[0];
	MousePosRelLast[1] = MousePosRel[1];
	MousePosRel[0] = (e->motion.x - ViewportPos[0]) / CurrViewportZoom;
	MousePosRel[1] = ((e->motion.y + ViewportPos[1]) - (WindowDims[1] - ViewportSize[1])) / CurrViewportZoom;

	if (Tool == TOOL_PAN) {
		ViewportPos[0] -= MousePosLast[0] - MousePos[0];
		ViewportPos[1] += MousePosLast[1] - MousePos[1];
	}

	MutateCanvas(false);
}

static inline void OnEvent_MouseButtonDown(SDL_Event* e) {
	if (e->button.button == SDL_BUTTON_LEFT) {
		MousePosDown[0] = MousePos[0];
		MousePosDown[1] = MousePos[1];
		MousePosDownRel[0] = MousePosRel[0];
		MousePosDownRel[1] = MousePosRel[1];

		IsLMBDown = true;
		if (CanMutateCanvas()) MutateCanvas(false);
	}
}

static inline void OnEvent_MouseButtonUp(SDL_Event* e) {
	if (e->button.button == SDL_BUTTON_LEFT) {
		IsLMBDown = false;
		if (CanMutateCanvas()) MutateCanvas(true);
	}
}

static inline void ProcessEvents(SDL_Window* window) {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		ImGui_ImplSDL2_ProcessEvent(&event);
		switch (event.type) {
			case SDL_QUIT:
				ShouldClose = true;
				break;
			case SDL_WINDOWEVENT:
				if (event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) {
					ShouldClose = true;
				}
				break;
			case SDL_KEYDOWN:
				OnEvent_KeyDown(&event);
				break;
			case SDL_KEYUP:
				OnEvent_KeyUp(&event);
				break;
			case SDL_MOUSEWHEEL:
				OnEvent_MouseWheel(&event);
				break;
			case SDL_MOUSEMOTION:
				OnEvent_MouseMotion(&event);
				break;
			case SDL_MOUSEBUTTONDOWN:
				OnEvent_MouseButtonDown(&event);
				break;
			case SDL_MOUSEBUTTONUP:
				OnEvent_MouseButtonUp(&event);
				break;
		}
	}

	if (CanvasDidMutate == true && IsLMBDown == false) {
		SaveHistory(&CURR_CANVAS_LAYER->history, CanvasDims[0] * CanvasDims[1] * 4 * sizeof(uchar_t), CURR_CANVAS_LAYER->pixels);
		CanvasDidMutate = false;
	}
}

void UpdateViewportSize() {
	ViewportSize[0] = CanvasDims[0] * CurrViewportZoom;
	ViewportSize[1] = CanvasDims[1] * CurrViewportZoom;
}

void UpdateViewportPos() {
	ViewportPos[0] = WindowDims[0] / 2 - CanvasDims[0] * CurrViewportZoom / 2;
	ViewportPos[1] = WindowDims[1] / 2 - CanvasDims[1] * CurrViewportZoom / 2;
}

void ZoomOpenGlViewport(int increase) {
	if (CanvasLocked) return;
	if (increase > 0) {
		if (CurrViewportZoom < 1.0f) CurrViewportZoom += 0.25f;
		else CurrViewportZoom += 1.0f;
	} else {
		if (CurrViewportZoom > 0.1f) {
			if (CurrViewportZoom < 1.0f) CurrViewportZoom -= 0.25f;
			else CurrViewportZoom -= 1.0f;
		}
	}
	if (CurrViewportZoom < 0.5f) CurrViewportZoom = 0.5f;
	else if (CurrViewportZoom > 100.0f) CurrViewportZoom = 100.0f;

	// This Ensures That The Canvas Is Zoomed From It's Center And Not From The Bottom Left Position
	int32_t CurrRectCenter[2] = { (ViewportSize[0] / 2) + ViewportPos[0], (ViewportSize[1] / 2) + ViewportPos[1] };
	int32_t NewRectCenter[2] = {
		(int32_t)(CanvasDims[0] * CurrViewportZoom / 2) + ViewportPos[0],
		(int32_t)(CanvasDims[1] * CurrViewportZoom / 2) + ViewportPos[1]
	};
	ViewportPos[0] -= NewRectCenter[0] - CurrRectCenter[0];
	ViewportPos[1] -= NewRectCenter[1] - CurrRectCenter[1];
	UpdateViewportSize();
}

uchar_t* GetPixel(int x, int y) {
	if (CURR_CANVAS_LAYER == NULL) return NULL;
	return CURR_CANVAS_LAYER->pixels + ((y * CanvasDims[0] + x) * 4);
}

static void InitWindowIcon(SDL_Window* window) {
	uchar_t* winIcon = (uchar_t*)Assets_Get("data/icons/icon-24.png", NULL);
	SDL_Surface* surface = SDL_CreateRGBSurfaceFrom(
		winIcon,
		24, 24, 32, 24 * 4,
		0x000000ff,
		0x0000ff00,
		0x00ff0000,
		0xff000000
	);
	if (surface == NULL) {
		Logger_Error("Failed to set window icon: %s", SDL_GetError());
		return;
	}
	SDL_SetWindowIcon(window, surface);
	SDL_FreeSurface(surface);
}

static void OpenNewFile(SDL_Window* window) {
	auto selection = pfd::open_file(
		"Select a file", ".",
		{
			"Image Files", "*.png *.jpg *.jpeg *.bmp",
			"All Files", "*"
		},
		pfd::opt::none
	).result();
	const char* _fName = selection.empty() ? NULL : selection[0].c_str();

	if (_fName != NULL) {
		int w = 0, h = 0, channels = 0;
		uchar_t* _data = stbi_load(_fName, &w, &h, &channels, 4);
		if (w > 0 && h > 0) {
			for (uint32_t i = 0; i < MAX_CANVAS_LAYERS; ++i) {
				if (CanvasLayers[i] != NULL) {
					Canvas_DestroyLayer(CanvasLayers[i]);
					CanvasLayers[i] = NULL;
				}
			}
			if ((uint32_t)w != CanvasDims[0] || (uint32_t)h != CanvasDims[1]) { // If The Image We Are Opening Doesn't Has Same Resolution As Our Current Image Then Resize The Canvas
				Canvas_Resize(w, h);
				CanvasDims[0] = w;
				CanvasDims[1] = h;
				CurrViewportZoom = 1.0f;
				UpdateViewportSize();
				UpdateViewportPos();
			}

			SelectedLayerIndex = 0;
			CURR_CANVAS_LAYER = Canvas_CreateLayer();
			memcpy(CURR_CANVAS_LAYER->pixels, _data, w * h * 4 * sizeof(uchar_t));
			FreeHistory(&CURR_CANVAS_LAYER->history);
			SaveHistory(&CURR_CANVAS_LAYER->history, w * h * 4 * sizeof(uchar_t), CURR_CANVAS_LAYER->pixels);

			snprintf(FilePath, SYS_PATH_MAX_SIZE, "%s", _fName);
			char* filePathBasename = Sys_GetBasename(_fName);
			snprintf(FileName, SYS_PATH_MAX_SIZE, "%s", filePathBasename);
			free(filePathBasename);
			stbi_image_free(_data);
			UpdateWindowTitle = true;
		}
	}
}

