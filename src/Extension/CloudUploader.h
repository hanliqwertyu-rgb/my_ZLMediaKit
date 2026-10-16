/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef SRC_EXTENSION_CLOUDUPLOADER_H_
#define SRC_EXTENSION_CLOUDUPLOADER_H_

namespace mediakit {

// 注册云存储上传事件监听，监听 Broadcast::kBroadcastRecordMP4 / kBroadcastRecordTs。
// 录制文件生成后，会把文件上传到 Cloud::kUrl 配置的地址。
// Register cloud storage upload listeners for MP4 / TS record completion events.
void installCloudUploader();

// 注销云存储上传事件监听。
// Unregister cloud storage upload listeners.
void uninstallCloudUploader();

} // namespace mediakit

#endif // SRC_EXTENSION_CLOUDUPLOADER_H_
