#pragma once

#include "ps2x/iop/iop_pad.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ps2_host_input
{
    constexpr uint32_t kPadPorts = 2u;
    // Two controller ports, each with a multitap (PS2X_MULTITAP): up to 8 players.
    constexpr uint32_t kMaxPlayers = 8u;

    void setFieldCounter(const std::atomic<uint64_t> *counter);
    void sample();
    void releaseMouse();
    // The pad at a controller port and multitap slot (0 = A, the pad plugged straight in).
    [[nodiscard]] ps2x::iop::PadInputState padState(uint32_t port, uint32_t slot = 0u);
    // The motors the game set on that pad (any thread); played on the controllers of its LargeMotor/SmallMotor bindings.
    void setVibration(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor);
    // While blocked (the in-game menu is open) every pad stays plugged in but idle, so the game keeps running.
    void setBlocked(bool blocked);
    // While set (the in-game menu is open and lets the game keep the controllers and keys), the mouse is the menu's:
    // no mouse camera, no mouse-button bindings, the cursor stays free.
    void setMenuHasMouse(bool menu);
    // The game's own reshaping of what a player's controls produce (e.g. a modern control scheme): called on the
    // window thread by sample() for every player that is not idle, after the bindings, before the game reads the
    // state. nullptr = none. The function must be cheap and must not call back into this namespace.
    using PadFilter = void (*)(uint32_t player, ps2x::iop::PadInputState &state);
    void setPadFilter(PadFilter filter);
    // The menu layer (K, Nov 14: the gameplay mapping changes only Gene in play). While on, player 1's controller
    // bindings are the default layout's and the keyboard and mouse bindings stay as bound, so every screen but play
    // (menus, dialogs, the roulette, minigames, the port's own screens) sees the controller as if it were never
    // remapped. A button held when the layer flips that now gives another PS2 button is dropped until released. The
    // game layer sets it (any thread); on until then. menuLayer(): the layer the last sample was built with.
    void setMenuLayer(bool on);
    [[nodiscard]] bool menuLayer();
    // The keyboard in the menu layer (K, Nov 16): fixed keys on top of player 1's keyboard bindings. Arrows and WASD
    // the d-pad (WASD stay the left stick too), Enter / Space the menus' confirm button(s), Esc / Backspace their cancel
    // button(s), Q / E L1 / R1; the play bindings of those keys and every key on the d-pad are dropped there (so the
    // style keys 1-4 do not move cursors). okButtons / cancelButtons: PadButton masks by Menu Controls (any thread;
    // applied at the next sample). escStart: Esc also presses Start (no sub-screen up: cutscenes, the title, dialogs).
    void setMenuKeys(uint16_t okButtons, uint16_t cancelButtons);
    void setMenuEscStart(bool on);
    // Gene's roulette is open (K, Nov 16): in the menu layer the left click is the OK button(s), the right click the
    // cancel button (Esc cancels too), W / S step it (the d-pad), A / D cycle the target (Target Select's d-pad); the
    // mouse wheel steps it whatever device is in use (a d-pad up / down tap per notch). Any thread.
    void setMenuRoulette(bool open);
    // A digit key 0-4 pressed while the menu layer was in use (the Techniques screen's direct style picks): 0..4, or
    // -1 for none since the last call. Any thread.
    [[nodiscard]] int takeMenuDigit();
    // Delete pressed while the menu layer was in use, since the last call (the keyboard page's Reset). Any thread.
    [[nodiscard]] bool takeMenuDelete();
    // Player 1's Dodge Modifier ([Pad1] DodgeModifier, Shift by default) was held at the last sample of play: the keys
    // and mouse buttons on the left stick moved the right stick instead (the game's dodges). For the pad filter (Modern
    // leaves that right stick to the game even in the free camera). Window thread (the filter), or any thread.
    [[nodiscard]] bool keyboardDodge();

    // Sweep CR-9: Windows' Sticky/Toggle/Filter Keys hotkeys are off while the game window has the focus (sample()
    // follows the focus). This puts the user's settings back for good: the window closed or the run ended. Any thread.
    void restoreShortcutKeys();

    // The bindings model (controls.ini; the launcher edits it; window thread only). Keys are PCSX2's [PadN] keys ("Cross",
    // "LUp"), bindings PCSX2 strings ("Keyboard/K", "SDL-0/FaceSouth", "Pointer-0/LeftButton").
    namespace controls
    {
        enum class Kind
        {
            Keys,       // one keyboard key or mouse button
            Controller, // one controller button or axis half
            Other       // a chord or a binding this build cannot read (kept as written)
        };
        [[nodiscard]] const std::string &path(); // empty: no file, edits last until the game closes
        [[nodiscard]] bool dirty();
        [[nodiscard]] const std::string &fileError(); // why the file could not be read (empty: it was read)
        [[nodiscard]] bool envOverride(uint32_t pad); // PS2X_PAD<n> in the environment wins over the file
        [[nodiscard]] std::vector<std::string> bindings(uint32_t pad, std::string_view key);
        void setBindings(uint32_t pad, std::string_view key, std::vector<std::string> list); // live at once, unsaved
        void resetDefaults(uint32_t pad);
        // Writes the file when something changed (tmp + replacing rename, retried); false = still unsaved.
        bool save(std::string &message);
        [[nodiscard]] Kind kindOf(std::string_view binding);
        [[nodiscard]] bool sameBinding(std::string_view a, std::string_view b);
        [[nodiscard]] std::string displayName(std::string_view binding);
        [[nodiscard]] std::string keyBinding(int raylibKey); // empty: not bindable (Esc, F-keys, unnamed keys)
        [[nodiscard]] const std::vector<int> &bindableKeys();
        [[nodiscard]] std::string mouseBinding(int button);
        [[nodiscard]] std::string padButtonBinding(int device, int button);
        [[nodiscard]] std::string padAxisBinding(int device, int axis, bool negative);
        // The PS2 button or stick direction ("Cross", "LUp") a keyboard key is bound to on this pad; empty = none.
        // For the hotkeys' clash warning. Window thread only (the bindings are edited there).
        [[nodiscard]] std::string keyboardKeyUse(uint32_t pad, int raylibKey);
        // Tests: whether the live table holds that binding; a file's bindings for one key; the default layout.
        [[nodiscard]] bool liveBinding(uint32_t pad, std::string_view key, std::string_view binding);
        bool fileBindings(const std::string &file, uint32_t pad, std::string_view key, std::vector<std::string> &out, std::string &error);
        [[nodiscard]] std::string defaultsDump(uint32_t pad);
        // Any thread (the in-game BUTTON MAPPING page runs on the game thread): queued for the window thread's next
        // sample(), which applies it to the live table and saves the file at once. Inputs are prompts::Input values.
        // Swap: the two PS2 buttons trade their controller bindings (each keeps its keyboard ones, in place). Reset:
        // every controller binding back to the default layout's; keyboard and mouse bindings and chords stay.
        void requestControllerSwap(uint32_t pad, uint8_t inputA, uint8_t inputB);
        void requestControllerReset(uint32_t pad);
        // The in-game keyboard page (K, Nov 16; any thread, queued like the swaps). KeyBind: `binding` (a key or mouse
        // button, "Keyboard/J") becomes the [PadN] key's first keyboard / mouse binding; any other key that held it
        // gives it up, and the one whose first it was takes the replaced binding instead (a swap). KeyboardReset: every
        // keyboard and mouse binding (and the Dodge Modifier) back to the default layout's; controller bindings stay.
        void requestKeyBind(uint32_t pad, std::string_view key, std::string binding);
        void requestKeyboardReset(uint32_t pad);
        // A key capture for that page (any thread): from begin, player 1's input is held from the game, the keys and
        // buttons down then must go up first, and the next bindable key or mouse button pressed is the result (Esc,
        // or the window losing the focus, cancels); the game gets nothing until everything is up again.
        // captureResult: 0 still waiting (or none begun), 1 captured (binding set), 2 cancelled. Taken once.
        void beginKeyCapture();
        void cancelKeyCapture();
        [[nodiscard]] int captureResult(std::string &binding);
    }

    // Player 1's extra buttons (K, Nov 14): Select and the back paddles. Each holds nothing, a copy of a game action
    // (the ten remappable PS2 buttons: the button added to that PS2 button's controller bindings, so it works in play
    // only: the menu layer has the default layout) or a command (save states; commands work everywhere). Commands are
    // [PadN] keys of controls.ini (SelectCommand, L4Command, R4Command, L5Command, R5Command = None / QuickSave /
    // QuickLoad / SaveLoad; Select defaults to SaveLoad, the paddles to None), so the launcher, the page and a reset
    // keep them with the bindings. Swaps leave copies where they are (a copy follows its action); a reset removes them.
    namespace extra
    {
        enum Button : uint8_t
        {
            Select,
            L4, // SDL's LeftPaddle1 (an Elite's P3, a DualSense Edge's left back button, a Steam pad's L4)
            R4, // RightPaddle1 (Elite P1)
            L5, // LeftPaddle2 (Elite P4)
            R5, // RightPaddle2 (Elite P2)
            Count
        };
        enum class Command : uint8_t
        {
            None,
            QuickSave,
            QuickLoad,
            SaveLoad, // tap (released without Start) saves; held + Start loads, that Start hidden from the game
            Count
        };
        constexpr uint8_t kNoCopy = 0xFFu;

        struct State
        {
            uint32_t generation = 0u; // bumped by every bindings rebuild that changed something here
            std::array<Command, Count> command{};
            std::array<uint8_t, Count> copy{};       // the prompts::Input (0..9) a copy of, kNoCopy: none
            std::array<uint8_t, 10> copies{};        // per remappable input: its controller bindings that are copies
            bool r3QuickTurn = false;                // [PadN] R3QuickTurn: Modern: Hold's R3 turns 180 degrees
        };
        [[nodiscard]] State state(); // any thread
        // The paddles player 1's controller reports (bit per Button; Select never counted): from the hardware, any
        // thread. 0 for a plain pad, Steam Input's virtual pad, GLFW, or no controller.
        [[nodiscard]] uint8_t paddles();
        // Any thread: queued for the window thread's next sample(), applied and saved at once. copyOf: a
        // prompts::Input 0..9 (command ignored), or kNoCopy for the command (None clears the button).
        void requestAssign(uint32_t pad, Button button, Command command, uint8_t copyOf = kNoCopy);
        // Any thread, queued like requestAssign: Modern: Hold's R3 (the PS2 R3 input) turns 180 degrees, or does nothing.
        void requestR3QuickTurn(uint32_t pad, bool on);
        // Window thread, after sample(): a save-state command an extra button fired (QuickSave / QuickLoad; None:
        // nothing), once.
        [[nodiscard]] Command takeFired();
        // Any thread: the commands are held (the game's pause menu and the title, K, Nov 21: a tap there is a menu press,
        // not a save). A press begun while held fires nothing, on release either; Start passes to the game.
        void setHeld(bool held);
    }

    // Button prompts (any thread). What player 1's bindings show where the game draws a PS2 button: for each input the
    // first keyboard/mouse binding and the first controller binding, i.e. the Controls screen's Key 1 and Controller
    // columns, taken from the live table (so PS2X_PAD1 in the environment counts too). Republished by every bindings
    // rebuild: the file load, the 1-s file poll and reset. Readers compare generation() with the one they
    // last saw and take snapshot() only when it moved; nothing here allocates.
    namespace prompts
    {
        // The 24 PS2 inputs, in Tokyo Jungle's pad_prompt_art.h PadPromptButton order.
        enum Input : uint8_t
        {
            Cross,
            Circle,
            Square,
            Triangle,
            L1,
            R1,
            L2,
            R2,
            L3,
            R3,
            Start,
            Select,
            Up,
            Down,
            Left,
            Right,
            LUp,
            LDown,
            LLeft,
            LRight,
            RUp,
            RDown,
            RLeft,
            RRight,
            InputCount
        };

        enum class Device : uint8_t
        {
            Keys,      // keyboard and mouse
            Controller
        };

        enum class PadStyle : uint8_t
        {
            Unknown, // no controller yet, or a name that says neither (positional layout: draw it like Xbox)
            Xbox,
            PlayStation,
            Nintendo,
            Steam
        };

        enum class Type : uint8_t
        {
            None, // nothing of that kind is bound to the input (labels empty)
            Key,
            Mouse,
            PadButton,
            PadAxis
        };

        // Key codes for "Keyboard/Shift", "Control", "Alt" (either side); every other key is its raylib KEY_* code.
        constexpr int16_t kKeyAnyShift = -1;
        constexpr int16_t kKeyAnyControl = -2;
        constexpr int16_t kKeyAnyAlt = -3;
        constexpr size_t kLabelBytes = 8u; // UTF-8, with the terminating 0

        struct Binding
        {
            Type type = Type::None;
            int8_t device = -1; // controller number (SDL-<n>); -1 for keys and mouse buttons
            int8_t sign = 0;    // PadAxis: +1 the positive half (a whole trigger), -1 the negative half
            int16_t code = 0;   // raylib KEY_* (or kKeyAny*), MOUSE_BUTTON_*, ps2_host_gamepads::Button or ::Axis
            // Short names: "F", "Space", "Ctrl", "Bksp", "Num8", "M1" (mouse left), "A", "LB", "LT", "View", "Menu".
            // Arrow keys, D-pad and stick directions use the arrows U+2190-U+2193 (up = "\xE2\x86\x91"; D-pad up is
            // "D" + up arrow, left stick left "LS" + left arrow), which pad_prompt_art draws as vector arrows.
            char label[kLabelBytes]{};
            char compact[kLabelBytes]{}; // at most 4 characters, for small cells: "Spc", "Shft", "N8"
        };

        struct Snapshot
        {
            uint32_t generation = 0u; // 0: not published yet
            std::array<Binding, InputCount> keys{};
            std::array<Binding, InputCount> controller{}; // Xbox-style names whatever the pad (see padStyle())
            // The keys in the menu layer (setMenuKeys): per input, the play key when it still gives that input there,
            // else the first fixed menu key that does (Enter, Esc, an arrow, Q / E). For the menus' button art.
            std::array<Binding, InputCount> menuKeys{};
            Binding dodgeModifier{}; // [Pad1] DodgeModifier's first key (None: no modifier)
            // How many controller bindings (a controller button or axis on its own, or a chord with one) each input has:
            // the BUTTON MAPPING page edits only a layout where each of its buttons has exactly one.
            std::array<uint8_t, InputCount> controllerCount{};
        };

        [[nodiscard]] uint32_t generation(); // bumped when a rebuild changed any label (never 0 once published)
        [[nodiscard]] Snapshot snapshot();   // a copy (~1 KB) under a short lock
        // The device player 1 last pressed a bound input with (past half: stick drift and unbound buttons never count),
        // at most one change per 500 ms, only while the window has focus. Starts as Keys.
        [[nodiscard]] Device lastDevice();
        // A case replay (test_mode): the device stays `device` whatever is pressed or connected, so menus and HUD hints
        // (both follow the device) replay the same every run (Nov 19: a controller seen at start in one run of two).
        void pinDevice(Device device);
        // From the name of the controller that last pressed a bound input (before any press: player 1's first
        // controller binding's controller); Unknown while that controller is not connected.
        [[nodiscard]] PadStyle padStyle();
    }
}
