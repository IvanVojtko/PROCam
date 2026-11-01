// SPDX-FileCopyrightText: 2013 - 2021 Jolla Ltd.
// SPDX-FileCopyrightText: 2025 Jolla Mobile Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

import QtQuick 2.0
import QtTest 1.0
import Sailfish.Silica 1.0
import com.procam.camera 1.0

Item {
    id: main

    readonly property var fileNames1: [
        "captures1/20210514_160038.jpg",
        "captures1/20210514_152532.jpg",
        "captures1/20210514_152221.jpg",
        "captures1/20210514_112217.jpg",
        "captures1/20210504_165124.jpg",
        "captures1/20210424_154127.jpg",
        "captures1/20210424_154116.jpg"
    ]
    readonly property var fileNames2: [
        "captures2/20210521_091613.jpg",
        "captures2/20210514_145420.jpg",
        "captures2/20210514_145049.jpg"
    ]

    readonly property var fileNames3: [
        "captures3/20210514_151510.jpg",
        "captures3/20210514_151454.jpg",
        "captures3/20210514_151437.jpg",
        "captures3/20210514_151435.jpg",
        "captures3/20210514_151048.jpg",
        "captures3/20210504_165204.jpg",
        "captures3/20210504_165138.jpg"
    ]


    width: 540
    height: 960

    CaptureModel {
        id: captureModel
    }

    Item {
        Repeater {
            id: repeater

            model: captureModel
            delegate: Item {
                property string url: model.url
                property string mimeType: model.mimeType
            }
        }
    }

    TestCase {

        function init() {
            captureModel.directories = []
            tryCompare(captureModel, "count", 0)
            tryCompare(repeater, "count", 0)
        }

        function test_directories() {
            var i
            var item

            captureModel.directories = [
                "/opt/tests/procam/auto/captures1"
            ]

            tryCompare(captureModel, "count", fileNames1.length)
            tryCompare(repeater, "count", fileNames1.length)

            for (i = 0; i < fileNames1.length; ++i) {
                item = repeater.itemAt(i)
                verify(item)

                compare(item.url, "file:///opt/tests/procam/auto/" + fileNames1[i])
            }

            captureModel.directories = [
                "/opt/tests/procam/auto/captures2"
            ]


            tryCompare(captureModel, "count", fileNames2.length)
            tryCompare(repeater, "count", fileNames2.length)

            for (i = 0; i < fileNames2.length; ++i) {
                item = repeater.itemAt(i)
                verify(item)

                compare(item.url, "file:///opt/tests/procam/auto/" + fileNames2[i])
            }

            captureModel.directories = [
                "/opt/tests/procam/auto/captures3"
            ]

            tryCompare(captureModel, "count", fileNames3.length)
            tryCompare(repeater, "count", fileNames3.length)

            for (i = 0; i < fileNames3.length; ++i) {
                item = repeater.itemAt(i)
                verify(item)

                compare(item.url, "file:///opt/tests/procam/auto/" + fileNames3[i])
            }

            captureModel.directories = [
                "/opt/tests/procam/auto/captures1",
                "/opt/tests/procam/auto/captures3"
            ]

            var fileNames = fileNames1.concat(fileNames3).sort(function (left, right) { return -left.slice(11).localeCompare(right.slice(11)) })

            tryCompare(captureModel, "count", fileNames.length)
            tryCompare(repeater, "count", fileNames.length)

            for (i = 0; i < fileNames.length; ++i) {
                item = repeater.itemAt(i)
                verify(item)

                compare(item.url, "file:///opt/tests/procam/auto/" + fileNames[i])
            }

            captureModel.directories = [
                "/opt/tests/procam/auto/captures1",
                "/opt/tests/procam/auto/captures2"
            ]

            fileNames = fileNames1.concat(fileNames2).sort(function (left, right) { return -left.slice(11).localeCompare(right.slice(11)) })

            tryCompare(captureModel, "count", fileNames.length)
            tryCompare(repeater, "count", fileNames.length)

            for (i = 0; i < fileNames.length; ++i) {
                item = repeater.itemAt(i)
                verify(item)

                compare(item.url, "file:///opt/tests/procam/auto/" + fileNames[i])
            }

            captureModel.directories = [
                "/opt/tests/procam/auto/captures2"
            ]

            tryCompare(captureModel, "count", fileNames2.length)
            tryCompare(repeater, "count", fileNames2.length)

            for (i = 0; i < fileNames2.length; ++i) {
                item = repeater.itemAt(i)
                verify(item)

                compare(item.url, "file:///opt/tests/procam/auto/" + fileNames2[i])
            }
        }

        function test_append() {
            var i
            var item

            captureModel.directories = [
                "/opt/tests/procam/auto/captures1"
            ]

            tryCompare(captureModel, "count", fileNames1.length)
            tryCompare(repeater, "count", fileNames1.length)

            for (i = 0; i < fileNames1.length; ++i) {
                item = repeater.itemAt(i)
                verify(item)

                compare(item.url, "file:///opt/tests/procam/auto/" + fileNames1[i])
            }

            var url = "file:///opt/tests/procam/auto/captures1/20300000_000000.jpg"
            var mimeType = "image/jpeg"

            captureModel.appendCapture(url, mimeType)

            compare(captureModel.count, fileNames1.length + 1)
            tryCompare(repeater, "count", fileNames1.length + 1)

            item = repeater.itemAt(0)
            verify(item)

            compare(item.url, url)
            compare(item.mimeType, mimeType)

            captureModel.deleteFile(0)

            tryCompare(captureModel, "count", fileNames1.length)
            tryCompare(repeater, "count", fileNames1.length)

            for (i = 0; i < fileNames1.length; ++i) {
                item = repeater.itemAt(i)
                verify(item)

                compare(item.url, "file:///opt/tests/procam/auto/" + fileNames1[i])
            }

            url = "file:///opt/tests/procam/auto/captures1/ProCam_20210514_160039.jpg"

            captureModel.appendCapture(url, mimeType)

            compare(captureModel.count, fileNames1.length + 1)
            tryCompare(repeater, "count", fileNames1.length + 1)

            item = repeater.itemAt(0)
            verify(item)

            compare(item.url, url)
            compare(item.mimeType, mimeType)

            captureModel.deleteFile(0)

            tryCompare(captureModel, "count", fileNames1.length)
            tryCompare(repeater, "count", fileNames1.length)
        }
    }
}
