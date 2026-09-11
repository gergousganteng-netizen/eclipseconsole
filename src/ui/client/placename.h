// placename.h -- PlaceId -> game-title lookup via the Roblox web API, and
// the WinHTTP GET it is built on (the offsets fetch shares it). Split out of
// eclipse_ui.cpp; the how-and-why lives in placename.cpp.
//
// The TU owns no state: resolved titles go to attach.cpp's place-title cache
// through place_title_remember, and results reach the page through ui_post.
#pragma once

#include <cstdint>
#include <string>

// WinHTTP GET. Empty string on any failure.
std::string http_get(const wchar_t* host, const std::wstring& path);

// Kick off the two-step lookup on a background thread; the result arrives
// as "PLACE_NAME:<placeId>|<name>" through ui_post.
void lookup_place_name_async(uint64_t place_id);

// A player's headshot URL for the clients panel, out of the same cache
// fx_avatar_url serves snippets from. Arrives as "AVATAR:<userId>|<url>";
// nothing is posted if there is no headshot, so the row keeps its initial.
void lookup_avatar_async(int64_t user_id);
