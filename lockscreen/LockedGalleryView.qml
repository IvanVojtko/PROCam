// SPDX-FileCopyrightText: 2018 - 2024 Jolla Ltd.
// SPDX-FileCopyrightText: 2024 - 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import QtQuick.Window 2.1
import Sailfish.Silica 1.0
import com.procam.camera 1.0
import Nemo.FileManager 1.0
import Nemo.DBus 2.0

GalleryView {
    id: root

    property bool hidden: window.Window.visibility === Window.Hidden

    onHiddenChanged: {
        if (hidden) {
            captureModel.clear()
            page.returnToCaptureMode()
        }
    }

    captureModel: ListModel {
        function appendCapture(url, mimeType) {
            insert(0, { url: url + "", mimeType: mimeType })
        }
        function deleteFile(index) {
            FileEngine.deleteFiles([get(index).url])
            remove(index)
        }
    }
    overlay.sharingAllowed: false
    overlay.ambienceAllowed: false
    overlay.additionalActions: IconButton {
        icon.source: "image://theme/icon-m-file-image?" + Theme.lightPrimaryColor
        onClicked: {
            var source = root.source + ""
            dbusGallery.call('openFile', source)
        }
    }

    ViewPlaceholder {
        text: "New photos and videos you take will appear here"
        hintText: "Unlock the device to access older photos and videos"
        enabled: count == 0
    }

    DBusInterface {
        id: dbusGallery

        service: "com.jolla.gallery"
        path: "/com/jolla/gallery/ui"
        iface: "com.jolla.gallery.ui"
    }
}
