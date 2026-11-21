#pragma once

// Canonical HEIC fixtures shared by the detector/decode group and the preview
// pipeline group. The valid fixture is the reference app's own e2e fixture
// (workspace_ui e0194ff e2e/mocks/fixtures/solid-color.heic,
// sha256 2a210a049069363a27fb14b0b01e909ac34625ac04a76f7b2f48196b3eb6aaa2):
// ftyp major "heic", 48x32, solid #4682b4. Embedded as base64 so the groups
// need no file I/O and the bytes stay review-inspectable.

#include <QByteArray>

inline QByteArray heicFixture()
{
    return QByteArray::fromBase64(
        "AAAAHGZ0eXBoZWljAAAAAG1pZjFoZWljbWlhZgAAAWltZXRhAAAAAAAAACFoZGxyAAAAAAAAAABwaWN0"
        "AAAAAAAAAAAAAAAAAAAAAA5waXRtAAAAAAABAAAAImlsb2MAAAAAREAAAQABAAAAAAGNAAEAAAAAAAAA"
        "HAAAACNpaW5mAAAAAAABAAAAFWluZmUCAAAAAAEAAGh2YzEAAAAA6WlwcnAAAADKaXBjbwAAAHZodmNDA"
        "QNwAAAAAAAAAAAAHvAA/P34+AAADwNgAAEAGEABDAH//wNwAAADAJAAAAMAAAMAHroCQGEAAQAqQgEBA3"
        "AAAAMAkAAAAwAAAwAeoCCBBZbq5Ka5uAhoMCAAAAMDIAAAAwAhYgABAAZEAcFzwIkAAAAUaXNwZQAAAAA"
        "AAABAAAAAQAAAAChjbGFwAAAAMAAAAAEAAAAgAAAAAf////AAAAAC////4AAAAAIAAAAQcGl4aQAAAAAD"
        "CAgIAAAAF2lwbWEAAAAAAAAAAQABBIECBIMAAAAkbWRhdAAAABgoAa8TgPgQ4af/6+or/iu1f9ZCb86KQz4=");
}

// A valid ftyp header followed by only a fragment of the container: it still
// SNIFFS as HEIC but libheif rejects the payload, so a surface must decide via
// the decode verdict (the reference previewFailed path).
inline QByteArray heicTruncatedFixture()
{
    return QByteArray::fromBase64(
        "AAAAHGZ0eXBoZWljAAAAAG1pZjFoZWljbWlhZgAAAWltZXRhAAAAAAAAACFoZGxyAAAAAAAAAABwaWN0"
        "AAAAAAAAAAAAAAAAAAAAAA5waXRtAAAAAAABAAAAImlsb2MA");
}

// The same container with a corrupted mdat payload: it still SNIFFS as HEIC
// (libheif does not validate the payload at container level), so surfaces must
// decide via the decode verdict, not the sniff - the reference split of
// isHeicAttachment vs previewFailed.
inline QByteArray heicCorruptFixture()
{
    return QByteArray::fromBase64(
        "AAAAHGZ0eXBoZWljAAAAAG1pZjFoZWljbWlhZgAAAWltZXRhAAAAAAAAACFoZGxyAAAAAAAAAABwaWN0"
        "AAAAAAAAAAAAAAAAAAAAAA5waXRtAAAAAAABAAAAImlsb2MAAAAAREAAAQABAAAAAAGNAAEAAAAAAAAA"
        "HAAAACNpaW5mAAAAAAABAAAAFWluZmUCAAAAAAEAAGh2YzEAAAAA6WlwcnAAAADKaXBjbwAAAHZodmNDA"
        "QNwAAAAAAAAAAAAHvAA/P34+AAADwNgAAEAGEABDAH//wNwAAADAJAAAAMAAAMAHroCQGEAAQAqQgEBA3"
        "AAAAMAkAAAAwAAAwAeoCCBBZbq5Ka5uAhoMCAAAAMDIAAAAwAhYgABAAZEAcFzwIkAAAAUaXNwZQAA/1o"
        "AAABAAAAAQAAAAChjbGFwAAAAMAAAAAEAAAAgAAAAAf////AAAAAC////4AAAAAIAAAAQcGl4aQAAAAAD"
        "CAgIAAAAF2lwbWEAAAAAAAAAAQABBIECBIMAAAAkbWRhdAAAABgoAa8TgPgQ4af/6+or/iu1f9ZCb86KQz4=");
}
