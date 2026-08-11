
//----------------------------------------------------------------------------
// Inline SVG icon markup, drawn in currentColor under a strict style-src CSP
// (ASCII-only source, no external assets). Top-bar icons mirror the app's:
// settings gear (Material), provider dropdown caret (ArrowDropDown), history
// back arrow and trash glyph; the Google "G" for sign-in; and the reply-footer
// dollar (stats) and copy glyphs.
//----------------------------------------------------------------------------

var SETTINGS_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" aria-hidden="true">' +
    '<path d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61' +
    'l-1.92-3.32c-.12-.22-.37-.29-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41' +
    '-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22' +
    'L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58c-.18.14-.23.41' +
    '-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84' +
    'c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22' +
    '.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"/></svg>';
var CARET_SVG =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor" aria-hidden="true">' +
    '<path d="M7 10l5 5 5-5z"/></svg>';
// Plus: the leading glyph on the drawer's "Switch account" row, which sits under the
// list of already-signed-in accounts. Those rows are plain labels, so the plus is what
// marks this one as "add another" rather than one more account to switch to. Sized
// down to 16 to sit with the 0.85rem account rows.
var PLUS_SVG =
    '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor" aria-hidden="true">' +
    '<path d="M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z"/></svg>';
// Plus IN A CIRCLE: the leading glyph on the drawer's "New chat" button. The bare
// plus above is the near-miss to avoid here -- with no circle it reads as "add an
// item to this list", which is what the list below it is. Harmony uses
// sys.symbol.plus_circle and android Icons.Outlined.AddCircleOutline for the same
// button; this is that glyph.
var PLUS_CIRCLE_SVG =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor" aria-hidden="true">' +
    '<path d="M13 7h-2v4H7v2h4v4h2v-4h4v-2h-4V7zm-1-5C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10' +
    'S17.52 2 12 2zm0 18c-4.41 0-8-3.59-8-8s3.59-8 8-8 8 3.59 8 8-3.59 8-8 8z"/></svg>';
// Hamburger: the top-left button that opens the nav drawer. A nav glyph rather than
// a person glyph because the drawer's primary content is the conversation list (its
// only scrolling band) -- account is one pinned row at the bottom. Same plain
// currentColor treatment and size as the settings gear, so the two bar actions read
// as a matched pair. android/ios still use their person icon; harmony already uses
// this one (sys.symbol.line_3_horizontal).
var MENU_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" aria-hidden="true">' +
    '<path d="M3 6h18v2H3V6zm0 5h18v2H3v-2zm0 5h18v2H3v-2z"/></svg>';
// History drawer icons: back arrow (close) and a delete/trash glyph.
var BACK_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" aria-hidden="true">' +
    '<path d="M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z"/></svg>';
// Trash: OUTLINED, matching android's Icons.Outlined.DeleteOutline and harmony's
// sys.symbol.trash on the same row. A solid bin (what this was) is a heavy mark to
// repeat down a list of conversations, and it was the only filled glyph in the
// drawer. Red is the callers' doing, not the glyph's -- it stays currentColor.
var TRASH_SVG =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor" aria-hidden="true">' +
    '<path d="M16 9v10H8V9h8m-1.5-6h-5l-1 1H5v2h14V4h-3.5l-1-1zM18 7H6v12c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7z"/></svg>';
// The Mirobody logo mark (assets/mirobody.svg, which stays the favicon). Inlined
// here so the upper glyph can follow the theme: it is drawn in currentColor --
// the caller tints it with `wordmark` (near-black / white), exactly as android's
// theme-aware `brand_logo_mark` does. The lower glyph keeps the fixed brand blue
// in both modes (docs/colors-and-fonts.md §5); it clears 3:1 on the dark page.
// Sized by the box it's placed in (widgets.brandMark), not by width/height here.
var MIROBODY_SVG =
    '<svg viewBox="0 0 23 24" width="100%" height="100%" fill="none" aria-hidden="true">' +
    '<path fill="currentColor" d="M22.8429 6.84462C22.8429 3.0773 19.9346 0.0197148 16.3398 0H0V10.3126H16.3398' +
    'C17.9718 10.3216 19.4609 10.9579 20.6013 12.0009C21.9741 10.7464 22.8429 8.90213 22.8429 6.84462Z"/>' +
    '<path fill="#005CF5" d="M20.6013 12.0018C19.4609 13.0449 17.9718 13.6811 16.3398 13.6883H0V24.0009H16.3398' +
    'C19.9346 23.9812 22.8429 20.9236 22.8429 17.1563C22.8429 15.0988 21.9741 13.2546 20.6013 12V12.0018Z"/></svg>';

// Google "G" mark (official four-color), used on the sign-in button.
var GOOGLE_SVG =
    '<svg viewBox="0 0 48 48" width="18" height="18" aria-hidden="true">' +
    '<path fill="#EA4335" d="M24 9.5c3.54 0 6.71 1.22 9.21 3.6l6.85-6.85C35.9 2.38 30.47 0 24 0 14.62 0 6.51 5.38 2.56 13.22l7.98 6.19C12.43 13.72 17.74 9.5 24 9.5z"/>' +
    '<path fill="#4285F4" d="M46.98 24.55c0-1.57-.15-3.09-.38-4.55H24v9.02h12.94c-.58 2.96-2.26 5.48-4.78 7.18l7.73 6c4.51-4.18 7.09-10.36 7.09-17.65z"/>' +
    '<path fill="#FBBC05" d="M10.53 28.59c-.48-1.45-.76-2.99-.76-4.59s.27-3.14.76-4.59l-7.98-6.19C.92 16.46 0 20.12 0 24c0 3.88.92 7.54 2.56 10.78l7.97-6.19z"/>' +
    '<path fill="#34A853" d="M24 48c6.48 0 11.93-2.13 15.89-5.81l-7.73-6c-2.15 1.45-4.92 2.3-8.16 2.3-6.26 0-11.57-4.22-13.47-9.91l-7.98 6.19C6.51 42.62 14.62 48 24 48z"/>' +
    '</svg>';

// Apple logo, used on the "Sign in with Apple" button. Single-color path drawn
// in currentColor (the button tints it black).
var APPLE_SVG =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor" aria-hidden="true">' +
    '<path d="M12.152 6.896c-.948 0-2.415-1.078-3.96-1.04-2.04.027-3.91 1.183-4.961 3.014-2.117 3.675-.546 9.103 1.519 12.09 1.013 1.454 2.208 3.09 3.792 3.039 1.52-.065 2.09-.987 3.935-.987 1.831 0 2.35.987 3.96.948 1.637-.026 2.676-1.48 3.676-2.948 1.156-1.688 1.636-3.325 1.662-3.415-.039-.013-3.182-1.221-3.22-4.857-.026-3.04 2.48-4.494 2.597-4.559-1.429-2.09-3.623-2.324-4.39-2.376-2-.156-3.675 1.09-4.61 1.09zM15.53 3.83c.843-1.012 1.4-2.427 1.245-3.83-1.207.052-2.662.805-3.532 1.818-.78.896-1.454 2.338-1.273 3.714 1.338.104 2.715-.688 3.559-1.701"/></svg>';

// WeChat logo (the two overlapping speech bubbles), used on the WeChat sign-in
// button. Single-color path drawn in currentColor (the button tints it green).
var WECHAT_SVG =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor" aria-hidden="true">' +
    '<path d="M8.691 2.188C3.891 2.188 0 5.476 0 9.53c0 2.212 1.17 4.203 3.002 5.55a.59.59 0 0 1 .213.665l-.39 1.48c-.019.07-.048.141-.048.213 0 .163.13.295.29.295a.326.326 0 0 0 .167-.054l1.903-1.114a.864.864 0 0 1 .717-.098 10.16 10.16 0 0 0 2.837.403c.276 0 .543-.027.811-.05-.857-2.578.157-4.972 1.932-6.446 1.703-1.415 4.882-1.932 7.621-1.073-.797-3.453-3.989-6.4-8.929-6.4zm-2.85 3.836c.629 0 1.139.51 1.139 1.139 0 .629-.51 1.139-1.139 1.139a1.139 1.139 0 0 1 0-2.278zm5.694 0c.629 0 1.139.51 1.139 1.139 0 .629-.51 1.139-1.139 1.139a1.139 1.139 0 0 1 0-2.278zm5.465 4.461c-4.143 0-7.5 2.766-7.5 6.177 0 3.411 3.357 6.177 7.5 6.177.857 0 1.679-.121 2.45-.341a.722.722 0 0 1 .597.082l1.642.949a.286.286 0 0 0 .146.044.252.252 0 0 0 .252-.252c0-.062-.024-.124-.04-.184l-.339-1.279a.508.508 0 0 1-.02-.139.504.504 0 0 1 .206-.407C22.029 20.292 24 17.939 24 15.162c0-3.411-3.357-6.677-7.5-6.677zm-2.478 3.515c.504 0 .912.408.912.912a.912.912 0 0 1-1.824 0c0-.504.408-.912.912-.912zm4.957 0c.504 0 .912.408.912.912a.912.912 0 0 1-1.824 0c0-.504.408-.912.912-.912z"/></svg>';

// GitHub "Octocat" mark, used on the "Continue with GitHub" button. Single-color
// path drawn in currentColor (the button tints it black).
var GITHUB_SVG =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor" aria-hidden="true">' +
    '<path d="M12 .297c-6.63 0-12 5.373-12 12 0 5.303 3.438 9.8 8.205 11.385.6.113.82-.258.82-.577 0-.285-.01-1.04-.015-2.04-3.338.724-4.042-1.61-4.042-1.61-.546-1.387-1.333-1.756-1.333-1.756-1.089-.745.083-.729.083-.729 1.205.084 1.839 1.237 1.839 1.237 1.07 1.834 2.807 1.304 3.492.997.107-.775.418-1.305.762-1.604-2.665-.305-5.467-1.334-5.467-5.931 0-1.311.469-2.381 1.236-3.221-.124-.303-.535-1.524.117-3.176 0 0 1.008-.322 3.301 1.23A11.509 11.509 0 0 1 12 5.803c1.02.005 2.047.138 3.006.404 2.291-1.552 3.297-1.23 3.297-1.23.653 1.653.242 2.874.118 3.176.77.84 1.235 1.911 1.235 3.221 0 4.609-2.807 5.624-5.479 5.921.43.372.823 1.102.823 2.222 0 1.606-.014 2.898-.014 3.293 0 .322.216.694.825.576C20.565 22.092 24 17.592 24 12.297c0-6.627-5.373-12-12-12"/></svg>';

// X (formerly Twitter) logo, used on the "Continue with X" button. Single-color
// path drawn in currentColor (the button tints it black).
var X_SVG =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor" aria-hidden="true">' +
    '<path d="M18.244 2.25h3.308l-7.227 8.26 8.502 11.24h-6.66l-5.214-6.817L4.99 21.75H1.68l7.73-8.835L1.254 2.25H8.08l4.713 6.231zm-1.161 17.52h1.833L7.084 4.126H5.117z"/></svg>';

// Tanka mark (stacked green/blue bars), used on the "Continue with Tanka"
// button. Multi-color like the Google "G": it carries its own fills and ignores
// the button's icon tint.
var TANKA_SVG =
    '<svg viewBox="0 0 34 30" width="18" height="18" aria-hidden="true">' +
    '<rect x="1" y="1" width="32" height="10" rx="4" fill="#22c55e"/>' +
    '<rect x="1" y="14" width="24" height="10" rx="4" fill="#1f6fff"/>' +
    '<path d="M6 24 v6 l7 -6 z" fill="#1f6fff"/></svg>';

// Footer action icons (Ant Design dollar, and a copy glyph), drawn in
// currentColor so the button color drives them.
var DOLLAR_SVG =
    '<svg viewBox="64 64 896 896" width="16" height="16" fill="currentColor" aria-hidden="true">' +
    '<path d="M512 64C264.6 64 64 264.6 64 512s200.6 448 448 448 448-200.6 448-448S759.4 64 512 64zm0 820c-205.4 0-372-166.6-372-372s166.6-372 372-372 372 166.6 372 372-166.6 372-372 372zm47.7-395.2l-25.4-5.9V348.6c38 5.2 61.5 29 65.5 58.2.5 4 3.9 6.9 7.9 6.9h44.9c4.7 0 8.4-4.1 8-8.8-6.1-62.3-57.4-102.3-125.9-109.2V263c0-4.4-3.6-8-8-8h-28.1c-4.4 0-8 3.6-8 8v33c-70.8 6.9-126.2 46-126.2 119 0 67.6 49.8 100.2 102.1 112.7l24.7 6.3v142.7c-44.2-5.9-69-29.5-74.1-61.3-.6-3.8-4-6.6-7.9-6.6H363c-4.7 0-8.4 4-8 8.7 4.5 55 46.2 105.6 135.2 112.1V761c0 4.4 3.6 8 8 8h28.4c4.4 0 8-3.6 8-8.1l-.2-31.7c78.3-6.9 134.3-48.8 134.3-124-.1-69.4-44.2-100.4-109-116.4zm-68.6-16.2c-5.6-1.6-10.3-3.1-15-5-33.8-12.2-49.5-31.9-49.5-57.3 0-36.3 27.5-57 64.5-61.7v124zM534.3 677V543.3c3.1.9 5.9 1.6 8.8 2.2 47.3 14.4 63.2 34.4 63.2 65.1 0 39.1-29.4 62.6-72 66.4z"/></svg>';
// Composer attachment icons: a paperclip (attach), a document glyph (non-image
// file chip), and a small close cross (remove an attachment). Drawn in
// currentColor like the rest.
var ATTACH_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" aria-hidden="true">' +
    '<path d="M16.5 6v11.5c0 2.21-1.79 4-4 4s-4-1.79-4-4V5c0-1.38 1.12-2.5 2.5-2.5s2.5 1.12 2.5 2.5' +
    'v10.5c0 .55-.45 1-1 1s-1-.45-1-1V6H10v9.5c0 1.38 1.12 2.5 2.5 2.5s2.5-1.12 2.5-2.5V5' +
    'c0-2.21-1.79-4-4-4S7 2.79 7 5v12.5c0 3.04 2.46 5.5 5.5 5.5s5.5-2.46 5.5-5.5V6h-1.5z"/></svg>';
var FILE_SVG =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor" aria-hidden="true">' +
    '<path d="M6 2c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V8l-6-6H6zm7 1.5L18.5 9H13V3.5z"/></svg>';
var CLOSE_SVG =
    '<svg viewBox="0 0 24 24" width="12" height="12" fill="currentColor" aria-hidden="true">' +
    '<path d="M18.3 5.71L12 12.01l-6.3-6.3-1.42 1.42 6.3 6.3-6.3 6.3 1.42 1.42 6.3-6.3 6.3 6.3 1.42-1.42-6.3-6.3 6.3-6.3z"/></svg>';
var COPY_SVG =
    // viewBox cropped toward the glyph's bounds (it's drawn inset in a 33x32
    // canvas); the padding keeps it a touch smaller than the stats icon.
    '<svg viewBox="6 5 21 21" width="20" height="20" fill="currentColor" aria-hidden="true">' +
    '<path d="M21.8088 20.7976H14.1441C13.7712 20.7976 13.4556 20.6685 13.1973 20.4101C12.939 20.1518 12.8098 19.8362 12.8098 19.4633V9.58429C12.8098 9.21143 12.939 8.89583 13.1973 8.6375C13.4556 8.37917 13.7712 8.25 14.1441 8.25H18.716C18.8939 8.25 19.065 8.28457 19.2292 8.3537C19.3933 8.42271 19.5359 8.51781 19.6571 8.63898L22.7542 11.736C22.8753 11.8572 22.9704 11.9998 23.0394 12.1639C23.1086 12.3282 23.1431 12.4992 23.1431 12.6771V19.4633C23.1431 19.8362 23.014 20.1518 22.7556 20.4101C22.4973 20.6685 22.1817 20.7976 21.8088 20.7976ZM22.036 12.6786H19.6088C19.3581 12.6786 19.1463 12.5922 18.9736 12.4195C18.8009 12.2468 18.7146 12.0351 18.7146 11.7844V9.35714H14.1441C14.0873 9.35714 14.0352 9.38082 13.988 9.42818C13.9406 9.47542 13.9169 9.52746 13.9169 9.58429V19.4633C13.9169 19.5202 13.9406 19.5722 13.988 19.6194C14.0352 19.6668 14.0873 19.6905 14.1441 19.6905H21.8088C21.8657 19.6905 21.9177 19.6668 21.965 19.6194C22.0123 19.5722 22.036 19.5202 22.036 19.4633V12.6786ZM11.1917 23.75C10.8189 23.75 10.5033 23.6208 10.2449 23.3625C9.98659 23.1042 9.85742 22.7886 9.85742 22.4157V13.2321C9.85742 13.0751 9.91044 12.9435 10.0165 12.8376C10.1224 12.7316 10.2539 12.6786 10.411 12.6786C10.5681 12.6786 10.6996 12.7316 10.8055 12.8376C10.9115 12.9435 10.9646 13.0751 10.9646 13.2321V22.4157C10.9646 22.4725 10.9882 22.5246 11.0356 22.5718C11.0828 22.6192 11.1349 22.6429 11.1917 22.6429H18.161C18.3181 22.6429 18.4496 22.6959 18.5555 22.8019C18.6615 22.9078 18.7146 23.0393 18.7146 23.1964C18.7146 23.3535 18.6615 23.485 18.5555 23.5909C18.4496 23.697 18.3181 23.75 18.161 23.75H11.1917Z"/></svg>';

// Share glyph (a node linked to two others), for the chat top-bar share action.
var SHARE_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" aria-hidden="true">' +
    '<path d="M18 16.08c-.76 0-1.44.3-1.96.77L8.91 12.7c.05-.23.09-.46.09-.7s-.04-.47-.09-.7' +
    'l7.05-4.11c.54.5 1.25.81 2.04.81 1.66 0 3-1.34 3-3s-1.34-3-3-3-3 1.34-3 3c0 .24.04.47.09.7' +
    'L8.04 9.81C7.5 9.31 6.79 9 6 9c-1.66 0-3 1.34-3 3s1.34 3 3 3c.79 0 1.5-.31 2.04-.81' +
    'l7.12 4.16c-.05.21-.08.43-.08.65 0 1.61 1.31 2.92 2.92 2.92s2.92-1.31 2.92-2.92-1.31-2.92-2.92-2.92z"/></svg>';
// People / care-circle glyph, for the settings menu entry.
var GROUP_SVG =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor" aria-hidden="true">' +
    '<path d="M16 11c1.66 0 2.99-1.34 2.99-3S17.66 5 16 5c-1.66 0-3 1.34-3 3s1.34 3 3 3zm-8 0' +
    'c1.66 0 2.99-1.34 2.99-3S9.66 5 8 5C6.34 5 5 6.34 5 8s1.34 3 3 3zm0 2c-2.33 0-7 1.17-7 3.5' +
    'V19h14v-2.5c0-2.33-4.67-3.5-7-3.5zm8 0c-.29 0-.62.02-.97.05 1.16.84 1.97 1.97 1.97 3.45V19' +
    'h6v-2.5c0-2.33-4.67-3.5-7-3.5z"/></svg>';

// Incognito ("privacy mode") ghost, for the top-bar toggle. A rounded dome with
// a scalloped hem and two eyes, drawn in currentColor so the button tints it.
// Two variants distinguish the toggle state: the SOLID (filled) ghost when
// incognito is ON, and the OUTLINE (hollow) ghost when OFF. The solid glyph is
// also reused (scaled up) for the empty-state hero and the active-session banner.
var INCOGNITO_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" aria-hidden="true">' +
    '<path d="M12 2C7.58 2 4 5.58 4 10v9.5c0 .84.98 1.3 1.63.77L7 19l1.63 1.3c.37.3.9.3 1.27 0' +
    'L12 18.5l2.1 1.8c.37.3.9.3 1.27 0L17 19l1.37 1.27c.65.53 1.63.07 1.63-.77V10c0-4.42-3.58-8-8-8z' +
    'm-2.5 9a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3zm5 0a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3z"/></svg>';
// Hollow (outline) counterpart, shown when incognito is OFF. Same 22px footprint
// as the solid glyph (its native viewBox is 20x20) so the toggle doesn't jump
// size when switching state. Body drawn as an even-odd ring (hollow shell) with
// two solid eyes.
var INCOGNITO_OUTLINE_SVG =
    '<svg viewBox="0 0 20 20" width="22" height="22" fill="currentColor" aria-hidden="true">' +
    '<path d="M6.99951 8.66672C7.5518 8.66672 7.99951 9.11443 7.99951 9.66672C7.9993 10.2188 7.55166 10.6667 6.99951 10.6667' +
    'C6.44736 10.6667 5.99973 10.2188 5.99951 9.66672C5.99951 9.11443 6.44723 8.66672 6.99951 8.66672Z"/>' +
    '<path d="M12.9995 8.66672C13.5518 8.66672 13.9995 9.11443 13.9995 9.66672C13.9993 10.2188 13.5517 10.6667 12.9995 10.6667' +
    'C12.4474 10.6667 11.9997 10.2188 11.9995 9.66672C11.9995 9.11443 12.4472 8.66672 12.9995 8.66672Z"/>' +
    '<path fill-rule="evenodd" clip-rule="evenodd" d="M10 2C14.326 2.00018 17.9998 5.67403 18 10V17.3123' +
    'C17.9997 17.5427 17.8411 17.8079 17.6172 17.8623C17.3932 17.9165 17.1614 17.7456 17.0557 17.5408' +
    'C16.7805 17.007 16.3658 16.5937 16.062 16.2878C15.7793 16.0034 15.4503 15.8338 14.9771 15.8337' +
    'C14.2092 15.8339 13.4371 16.3862 12.9487 17.53C12.8701 17.7138 12.6887 17.8621 12.4888 17.8623' +
    'C12.2888 17.8623 12.1076 17.7138 12.0288 17.53C11.5404 16.386 10.7674 15.8339 9.99951 15.8337' +
    'C9.23161 15.8339 8.45959 16.386 7.97119 17.53C7.89253 17.7138 7.71118 17.8621 7.51123 17.8623' +
    'C7.31122 17.8623 7.13006 17.7138 7.05127 17.53C6.56296 16.3862 5.78982 15.834 5.02197 15.8337' +
    'C4.54861 15.8338 4.21974 16.0032 3.93701 16.2878C3.63309 16.5937 3.21952 17.0715 2.94434 17.6055' +
    'C2.83865 17.8103 2.60589 17.9165 2.38184 17.8623C2.15801 17.8079 2.00033 17.6073 2 17.377V10' +
    'C2.00018 5.67403 5.67403 2.00018 10 2ZM10 3C6.22631 3.00018 3.00018 6.22631 3 10V15.8633' +
    'C3.0205 15.8414 3.20696 15.6049 3.22803 15.5837C3.67524 15.1336 4.251 14.8338 5.02197 14.8337' +
    'C6.03838 14.8341 6.90232 15.4025 7.51025 16.2937C8.11828 15.4018 8.9824 14.8338 9.99951 14.8337' +
    'C11.0163 14.8338 11.8798 15.4022 12.4878 16.2937C13.0959 15.4018 13.9601 14.8339 14.9771 14.8337' +
    'C15.7481 14.8338 16.3247 15.1336 16.772 15.5837C16.772 15.5837 16.9796 15.812 17 15.8337V10' +
    'C16.9998 6.22631 13.7737 3.00018 10 3Z"/></svg>';

// Up-arrow for the circular composer send button (drawn in currentColor /
// onPrimary). Stroke-based to read crisply at small sizes.
var SEND_ARROW_SVG =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" ' +
    'stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
    '<path d="M12 19V5"/><path d="M5 12l7-7 7 7"/></svg>';

//----------------------------------------------------------------------------

exports.SETTINGS_SVG = SETTINGS_SVG;
exports.CARET_SVG    = CARET_SVG;
exports.PLUS_SVG     = PLUS_SVG;
exports.PLUS_CIRCLE_SVG = PLUS_CIRCLE_SVG;
exports.MENU_SVG     = MENU_SVG;
exports.BACK_SVG     = BACK_SVG;
exports.TRASH_SVG    = TRASH_SVG;
exports.MIROBODY_SVG = MIROBODY_SVG;
exports.GOOGLE_SVG   = GOOGLE_SVG;
exports.APPLE_SVG    = APPLE_SVG;
exports.WECHAT_SVG   = WECHAT_SVG;
exports.GITHUB_SVG   = GITHUB_SVG;
exports.X_SVG        = X_SVG;
exports.TANKA_SVG    = TANKA_SVG;
exports.DOLLAR_SVG   = DOLLAR_SVG;
exports.COPY_SVG     = COPY_SVG;
exports.ATTACH_SVG   = ATTACH_SVG;
exports.FILE_SVG     = FILE_SVG;
exports.CLOSE_SVG    = CLOSE_SVG;
exports.SHARE_SVG    = SHARE_SVG;
exports.GROUP_SVG    = GROUP_SVG;
exports.INCOGNITO_SVG  = INCOGNITO_SVG;
exports.INCOGNITO_OUTLINE_SVG = INCOGNITO_OUTLINE_SVG;
exports.SEND_ARROW_SVG = SEND_ARROW_SVG;
