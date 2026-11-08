# CTest of restoring a backup from the settings menu (#192, save_menu.h, ADR 0078's addendum):
# save, play on, save again, restore the first backup through the menu's Backups list, reload.
#
#   cmake -DPROGRAM=<MapleThief> -DSAVE=<play.save_quit's data dir> -DOUT=<dir> -P restore_backup_test.cmake
#
# (platform.game_restore_backup, real assets, after play.save_quit) The game on SDL's dummy
# drivers, --fixed-step 16.667, all four runs on one data directory (a copy of play.save_quit's
# saves/: a hero in Henesys, 2.0). The saves are --autosave on_quit's (the game's own save
# pair at the quit; the backup hook keeps each distinct save, ADR 0078):
#   - first:   load slot 0 (taps on Play, the first slot and Start) and quit: the first save,
#              backups/0/ gets its first folder.
#   - second:  load, walk left for 8 s (the key pad's left arm held), quit: the second save, with
#              the hero elsewhere on the map; a second backup.
#   - field:   load, then --restore-backup 12,2 (the list's second row, the first save) with the
#              hero in the field: refused ("Restore from the title ..."), theif0 unchanged (no
#              autosave in this run).
#   - restore: at the title, --restore-backup 3,2: the menu's list row 2 is the first save
#              ("Slot 1  Lv <n> <job>"), asked and confirmed; theif0 and quest0 are the first
#              save's bytes again and backups/import/<time>/ holds the second save's.
#   - reload:  load slot 0 and quit: the game saves the hero it loaded, at the first save's level,
#              area, map and (within a few points) position, not the second's.
# Prints "skipped:" (the test's SKIP_REGULAR_EXPRESSION) when play.save_quit left no save.

if(NOT EXISTS "${SAVE}/data/saves/theif0")
    message(STATUS "skipped: no save in ${SAVE} (run play.save_quit first)")
    return()
endif()

file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")
set(ENV{SDL_VIDEO_DRIVER} dummy)
set(ENV{SDL_AUDIO_DRIVER} dummy)
set(ENV{MAPLETHIEF_LANGUAGE} en)
set(ENV{MAPLETHIEF_DATA_DIR} "${OUT}/data")
file(COPY "${SAVE}/data/saves" DESTINATION "${OUT}/data")
set(docs "${OUT}/data/saves")

# Runs the game with ${ARGN}; fails unless it exits 0. The log goes to <OUT>/<name>.log and is
# returned in <name>_log. skip_intros is pinned off as in autosave_test.cmake (it times ticks).
function(run name)
    execute_process(
        COMMAND "${PROGRAM}" --skip-intros off --fixed-step 16.667 ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    file(WRITE "${OUT}/${name}.log" "${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: exit ${result} (log: ${OUT}/${name}.log):\n${error}")
    endif()
    set(${name}_log "${error}" PARENT_SCOPE)
endfunction()

function(expect name text re)
    if(NOT text MATCHES "${re}")
        message(FATAL_ERROR "${name}: no '${re}' (log: ${OUT}/${name}.log)")
    endif()
endfunction()

# The hero's fields in a theif file (docs/formats/save.md): level 0x002, area 0x41C, map 0x41D,
# x 0x41E (i16, little-endian), as numbers in <prefix>_level, _area, _map, _x.
function(hero file prefix)
    file(READ "${file}" level OFFSET 2 LIMIT 1 HEX)
    file(READ "${file}" place OFFSET 1052 LIMIT 4 HEX) # 0x41C: area, map, x (2 bytes)
    string(SUBSTRING "${place}" 0 2 area)
    string(SUBSTRING "${place}" 2 2 map)
    string(SUBSTRING "${place}" 4 2 x_lo)
    string(SUBSTRING "${place}" 6 2 x_hi)
    math(EXPR level "0x${level}")
    math(EXPR area "0x${area}")
    math(EXPR map "0x${map}")
    math(EXPR x "0x${x_hi}${x_lo}")
    if(x GREATER 32767)
        math(EXPR x "${x} - 65536")
    endif()
    set(${prefix}_level ${level} PARENT_SCOPE)
    set(${prefix}_area ${area} PARENT_SCOPE)
    set(${prefix}_map ${map} PARENT_SCOPE)
    set(${prefix}_x ${x} PARENT_SCOPE)
endfunction()

set(load_taps --tap 3,106,165 --tap 5,99,232 --tap 6.5,420,79)

# first: the first save
run(first --autosave on_quit ${load_taps} --quit 16)
expect(first "${first_log}" "autosave: slot 0 saved at the quit")
file(SHA256 "${docs}/theif0" first_theif)
file(SHA256 "${docs}/quest0" first_quest)
file(COPY "${docs}/theif0" DESTINATION "${OUT}/first")
hero("${docs}/theif0" first)
file(GLOB slot_backups "${OUT}/data/backups/0/*")
list(LENGTH slot_backups n)
if(NOT n EQUAL 1)
    message(FATAL_ERROR "first: ${n} backups of slot 0, expected 1")
endif()

# second: play on (walk left), save again
run(second --autosave on_quit ${load_taps} --hold 12,35,257,8 --quit 24)
expect(second "${second_log}" "autosave: slot 0 saved at the quit")
file(SHA256 "${docs}/theif0" second_theif)
hero("${docs}/theif0" second)
if(second_theif STREQUAL first_theif)
    message(FATAL_ERROR "second: the save did not change (the hero did not move)")
endif()
math(EXPR moved "${first_x} - ${second_x}")
if(moved LESS 40)
    message(FATAL_ERROR "second: the hero moved ${moved} points left (x ${first_x} -> "
                        "${second_x}); expected a walk")
endif()
file(GLOB slot_backups "${OUT}/data/backups/0/*")
list(LENGTH slot_backups n)
if(NOT n EQUAL 2)
    message(FATAL_ERROR "second: ${n} backups of slot 0, expected 2")
endif()

# field: a character loaded: the menu's restore is refused, nothing changes
run(field ${load_taps} --restore-backup 12,2 --quit 14)
expect(field "${field_log}"
       "saves: --restore-backup: Restore from the title or the main menu \\(a character is loaded\\)")
if(EXISTS "${OUT}/data/backups/import")
    message(FATAL_ERROR "field: a restore with a character loaded backed something up")
endif()
file(SHA256 "${docs}/theif0" field_theif)
if(NOT field_theif STREQUAL second_theif)
    message(FATAL_ERROR "field: theif0 changed")
endif()

# restore: at the title, the list's second row (the first save), asked and confirmed
run(restore --restore-backup 3,2 --quit 5)
expect(restore "${restore_log}"
       "saves: --restore-backup at [0-9.]+ s: row 2 'Slot 1  Lv ${first_level} [A-Za-z ]+' \\([0-9: -]+\\): asks 'Restore slot 1 from [0-9: -]+\\? The current save is backed up first\\.'")
expect(restore "${restore_log}"
       "saves: --restore-backup: Restored slot 1 from [0-9: -]+ \\(the save it replaced is in backups/import\\)")
file(SHA256 "${docs}/theif0" restored_theif)
file(SHA256 "${docs}/quest0" restored_quest)
if(NOT restored_theif STREQUAL first_theif OR NOT restored_quest STREQUAL first_quest)
    message(FATAL_ERROR "restore: theif0 / quest0 are not the first save's bytes")
endif()
file(GLOB import_backups "${OUT}/data/backups/import/*")
list(LENGTH import_backups n)
if(NOT n EQUAL 1)
    message(FATAL_ERROR "restore: ${n} import backups, expected 1 (the replaced save)")
endif()
file(SHA256 "${import_backups}/theif0" replaced_theif)
if(NOT replaced_theif STREQUAL second_theif)
    message(FATAL_ERROR "restore: backups/import does not hold the save it replaced")
endif()

# reload: the hero is the first save's
run(reload --autosave on_quit ${load_taps} --quit 16)
expect(reload "${reload_log}"
       "autosave: slot 0 saved at the quit \\(SDL_EVENT_QUIT, scripted\\) \\(16\\.000 s\\): map ${first_area}\\.${first_map}")
hero("${docs}/theif0" reload)
math(EXPR off_first "${reload_x} - ${first_x}")
math(EXPR off_second "${reload_x} - ${second_x}")
if(off_first LESS 0)
    math(EXPR off_first "-${off_first}")
endif()
if(off_second LESS 0)
    math(EXPR off_second "-${off_second}")
endif()
if(NOT reload_level EQUAL first_level OR NOT reload_area EQUAL first_area OR NOT reload_map EQUAL
                                                                               first_map
   OR off_first GREATER 4 OR off_first GREATER_EQUAL off_second)
    message(FATAL_ERROR "reload: the hero is level ${reload_level} at map ${reload_area}.${reload_map} "
                        "x ${reload_x}; the first save: level ${first_level} at "
                        "${first_area}.${first_map} x ${first_x}; the second: x ${second_x}")
endif()

message(STATUS "restore: the first save (level ${first_level}, map ${first_area}.${first_map}, "
               "x ${first_x}) restored over the second (x ${second_x}) from the menu's list at "
               "the title (refused in the field); reloaded at x ${reload_x}")
