# <Subsystem>

<!-- The atlas page template (game-atlas, "Page per subsystem"). Copy it to mapping/atlas/<subsystem>.md.
     kmap check enforces the rules below; mapping/conventions.md ("Atlas pages") has them in full.
     - Keep the eight `## ` sections, in this order. Sub-sections (`###`) and extra `## ` sections after them are fine.
     - A qualified name in a code span (`cube::Creature`, `cube::Creature::update()`, `cube::CreatureData.hp`) is a
       reference: it must be the name or alias of a mapping/ entry. Write a name that isn't mapped yet as plain
       text, and say where it comes from (a string, cuwo, a guess).
     - Link a class or function to its generated reference page in docs/game/, for example
       [`cube::Creature`](../../docs/game/types/cube_Creature.md). tools/check_docs.py checks the links.
     - Cite evidence the way entries do: `probe:<id>`, `rtti:<class>`, a string, an import. -->

## Purpose

What the subsystem does for the player, in two or three sentences, and where it sits between the others.

## Key classes

The classes and types that carry the subsystem's state, each with its status (`verified`, `candidate`, `guess`) and
where the identification comes from (RTTI, strings, imports, correlation, probes).

## Entry points

The functions through which the rest of the game reaches the subsystem: per-frame updates, constructors, handlers,
virtual slots.

## Data flow

How state moves: who creates it, who updates it each frame, who reads it, what is saved or sent.

## Client and server

What runs in `Cube.exe`, what runs in `Server.exe` (and in the in-process server of single-player), and which side is
authoritative.

## Hook points of interest

Where a mod would hook to observe or change the subsystem, with the mapping entries and their status.

## Open questions

What isn't known yet, and how each question would be answered (a probe, a static trace, correlation with cuwo).

## Depth

not started
