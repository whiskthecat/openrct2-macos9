# Installing with a coding agent

If your Mac runs [AgentChat](https://github.com/whiskthecat/agentchat-macos9), a coding
agent on your modern computer (Claude Code, Codex and similar) can install the game for
you. It sends the files over the network, places them on the Mac and starts the game.

You need:

- AgentChat running on the Mac and connected to its relay (Docker);
- your agent connected to the relay's Mac tools, as described in the
  [AgentChat README](https://github.com/whiskthecat/agentchat-macos9#use-the-mac-from-your-own-coding-agent);
- `openrct2.pak` and `title.pak` from the release, and your game packs from
  `tools/make_packs.py`, on the computer that runs the relay. The application itself is
  downloaded from the release by the relay.

Then ask your agent: *"Install RollerCoaster Tycoon on my Mac by following
docs/install-with-an-agent.md."*

## Steps for the agent

Run the `docker compose` commands in the folder that holds AgentChat's `compose.yaml`. If
the relay is on another machine, run them there, for example over SSH. Mac paths use colons
(`Volume:Folder:File`), and HFS names are at most 31 characters.

1. **Check the Mac.** `imac_get_state` must report a connection. `imac_list_volumes`
   shows the disks: choose one with at least 450 MB free and remember its name.
   `imac_get_system_configuration` gives the physical RAM (see step 6).

2. **Install the application.** `imac_import_archive` with `url`
   `https://github.com/whiskthecat/openrct2-macos9/releases/download/v0.6.0/RCT.bin` and
   `destination` `<Volume>:RollerCoaster Tycoon` creates the game folder and places the
   application in it, with its Finder type and creator. (A local `RCT.bin` works too: copy
   it into the relay as in step 3 and pass `host_path` instead of `url`.)

3. **Copy the data into the relay.** The Mac tools read files on the relay's side, so
   first copy them into its container:

   ```sh
   docker compose exec -T relay mkdir -p /data/transfer
   docker compose cp openrct2.pak relay:/data/transfer/
   docker compose cp title.pak relay:/data/transfer/
   docker compose cp packs/. relay:/data/transfer/   # every game*.pak / rct1*.pak
   ```

   If your agent can't add an MCP server to its current session, it can run one tool per
   command instead: `docker compose exec -T relay python3 /app/bridge/mac_mcp.py --call
   imac_get_state '{}'`.

4. **Send each pack.** For every `.pak` file, call `imac_import_file` with
   `host_path` `/data/transfer/<name>`, `destination`
   `<Volume>:RollerCoaster Tycoon:<name>`, and `expected_sha256` set to the file's SHA-256.
   With AgentChat 98 or later on both the Mac and the relay, a 58 MB pack takes about 30
   seconds and RCT Classic's packs about four minutes in all; older versions are about
   three times slower.

5. **Set the volume.** The game reads launch options from `RCT.txt` beside the
   application. Write it with `imac_write_file`, containing `--volume=25` (or `--mute` if the
   user isn't at the Mac). Ask the user before choosing a louder setting: an iMac G3's
   speakers are very loud.

6. **Memory.** The game asks for 256 MB and starts in as little as 100 MB. If the Mac has
   less than about 384 MB of physical RAM, ask the user to select RCT, choose File → Get
   Info → Memory and lower Preferred Size, or to turn on virtual memory.

7. **Start it.** `imac_launch_application` with `<Volume>:RollerCoaster Tycoon:RCT`.
   The first launch indexes the game data for a minute or two. Its progress goes to
   `RCT.log` in the game folder (`imac_read_file`); when the scenario index is done (about
   90 seconds on a 450 MHz G3), the log shows `loading: title` and the title screen appears.
   `imac_screenshot` shows it.

8. **Quitting.** The game accepts commands through `imac_application_command`. Find its
   process with `imac_list_processes` (creator `ORCG`) and send `{"op":"quit"}` with its
   `high` and `low` numbers. The tool then reports that the application exited (Mac OS
   error -600): that is the expected answer. `{"op":"status"}` describes the running game.

9. **Clean up.** `docker compose exec -T relay rm -rf /data/transfer`.

If something fails, `RCT.log` and, after a crash, `RCT.err` in the game folder explain why.
Missing or damaged packs are listed near the top of `RCT.log`.
