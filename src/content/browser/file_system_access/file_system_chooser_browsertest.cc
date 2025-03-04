// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include "base/containers/contains.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/gmock_callback_support.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "content/browser/file_system_access/fake_file_system_access_permission_context.h"
#include "content/browser/file_system_access/file_system_access_manager_impl.h"
#include "content/browser/file_system_access/file_system_chooser_test_helpers.h"
#include "content/browser/file_system_access/fixed_file_system_access_permission_grant.h"
#include "content/browser/file_system_access/mock_file_system_access_permission_context.h"
#include "content/browser/file_system_access/mock_file_system_access_permission_grant.h"
#include "content/browser/renderer_host/back_forward_cache_disable.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/back_forward_cache_util.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/shell/browser/shell.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "storage/browser/file_system/external_mount_points.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_manager.mojom-shared.h"
#include "third_party/blink/public/mojom/frame/fullscreen.mojom.h"
#include "ui/shell_dialogs/select_file_dialog.h"
#include "ui/shell_dialogs/select_file_dialog_factory.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"

namespace content {

using base::test::RunOnceCallback;
using blink::mojom::PermissionStatus;
using SensitiveDirectoryResult =
    FileSystemAccessPermissionContext::SensitiveDirectoryResult;
using PathInfo = FileSystemAccessPermissionContext::PathInfo;
using PathType = FileSystemAccessPermissionContext::PathType;

static constexpr char kTestMountPoint[] = "testfs";

// This browser test implements end-to-end tests for the file picker
// APIs.
class FileSystemChooserBrowserTest : public ContentBrowserTest {
 public:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());

    // Register an external mount point to test support for virtual paths.
    // This maps the virtual path a native local path to make these tests work
    // on all platforms. We're not testing more complicated ChromeOS specific
    // file system backends here.
    storage::ExternalMountPoints::GetSystemInstance()->RegisterFileSystem(
        kTestMountPoint, storage::kFileSystemTypeLocal,
        storage::FileSystemMountOption(), temp_dir_.GetPath());

    ASSERT_TRUE(embedded_test_server()->Start());

    ContentBrowserTest::SetUp();
  }

  void SetUpCommandLine(base::CommandLine* command_line) override {
    // Enable experimental web platform features to enable write access.
    command_line->AppendSwitch(
        switches::kEnableExperimentalWebPlatformFeatures);
  }

  void TearDown() override {
    ContentBrowserTest::TearDown();
    storage::ExternalMountPoints::GetSystemInstance()->RevokeFileSystem(
        kTestMountPoint);
    ui::SelectFileDialog::SetFactory(nullptr);
    ASSERT_TRUE(temp_dir_.Delete());
  }

  bool IsFullscreen() {
    WebContents* web_contents = shell()->web_contents();
    return web_contents->IsFullscreen();
  }

  void EnterFullscreen() {
    WebContentsImpl* web_contents_impl =
        static_cast<WebContentsImpl*>(shell()->web_contents());
    web_contents_impl->EnterFullscreenMode(web_contents_impl->GetMainFrame(),
                                           {});
  }

  base::FilePath CreateTestFile(const std::string& contents) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    base::FilePath result;
    EXPECT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.GetPath(), &result));
    EXPECT_TRUE(base::WriteFile(result, contents));
    return result;
  }

  base::FilePath CreateTestDir() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    base::FilePath result;
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("test"), &result));
    return result;
  }

 protected:
  base::ScopedTempDir temp_dir_;
};

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, CancelDialog) {
  ui::SelectFileDialog::SetFactory(new CancellingSelectFileDialogFactory);
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  auto result = EvalJs(shell(), "self.showOpenFilePicker()");
  EXPECT_TRUE(result.error.find("aborted") != std::string::npos)
      << result.error;
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, OpenFile) {
  const std::string file_contents = "hello world!";
  const base::FilePath test_file = CreateTestFile(file_contents);
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_OPEN_FILE, dialog_params.type);
  EXPECT_EQ(shell()->web_contents()->GetTopLevelNativeWindow(),
            dialog_params.owning_window);
  EXPECT_EQ(
      file_contents,
      EvalJs(shell(),
             "(async () => { const file = await self.selected_entry.getFile(); "
             "return await file.text(); })()"));
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, OpenFileNonASCII) {
  const std::string file_contents = "hello world!";
  const base::FilePath test_file =
      temp_dir_.GetPath().Append(base::FilePath::FromUTF8Unsafe("😋.txt"));
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    EXPECT_TRUE(base::WriteFile(test_file, file_contents));
  }

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_OPEN_FILE, dialog_params.type);
  EXPECT_EQ(shell()->web_contents()->GetTopLevelNativeWindow(),
            dialog_params.owning_window);
  EXPECT_EQ(
      file_contents,
      EvalJs(shell(),
             "(async () => { const file = await self.selected_entry.getFile(); "
             "return await file.text(); })()"));
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, FullscreenOpenFile) {
  const std::string file_contents = "hello world!";
  const base::FilePath test_file = CreateTestFile(file_contents);
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EnterFullscreen();
  EXPECT_TRUE(IsFullscreen());
  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_FALSE(IsFullscreen());
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       OpenFile_BlockedPermission) {
  const base::FilePath test_file = CreateTestFile("Save File");
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  EXPECT_CALL(permission_context,
              CanObtainReadPermission(url::Origin::Create(
                  embedded_test_server()->GetURL("/title1.html"))))
      .WillOnce(testing::Return(false));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  auto result = EvalJs(shell(), "self.showOpenFilePicker()");
  EXPECT_TRUE(result.error.find("not allowed") != std::string::npos)
      << result.error;
  EXPECT_EQ(ui::SelectFileDialog::SELECT_NONE, dialog_params.type);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, OpenFile_ExternalPath) {
  const std::string file_contents = "hello world!";
  const base::FilePath test_file = CreateTestFile(file_contents);
  const base::FilePath virtual_path =
      base::FilePath::FromUTF8Unsafe(kTestMountPoint)
          .Append(test_file.BaseName());

  ui::SelectedFileInfo selected_file = {base::FilePath(), base::FilePath()};
  selected_file.virtual_path = virtual_path;

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({selected_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(virtual_path.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_OPEN_FILE, dialog_params.type);
  EXPECT_EQ(shell()->web_contents()->GetTopLevelNativeWindow(),
            dialog_params.owning_window);
  EXPECT_EQ(
      file_contents,
      EvalJs(shell(),
             "(async () => { const file = await self.selected_entry.getFile(); "
             "return await file.text(); })()"));
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, SaveFile_NonExistingFile) {
  const std::string file_contents = "file contents to write";
  const base::FilePath test_file = CreateTestFile("");
  {
    // Delete file, since SaveFile should be able to deal with non-existing
    // files.
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::DeleteFile(test_file));
  }
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showSaveFilePicker();"
                   "  self.entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_SAVEAS_FILE, dialog_params.type);
  EXPECT_EQ(static_cast<int>(file_contents.size()),
            EvalJs(shell(),
                   JsReplace("(async () => {"
                             "  const w = await self.entry.createWritable();"
                             "  await w.write(new Blob([$1]));"
                             "  await w.close();"
                             "  return (await self.entry.getFile()).size; })()",
                             file_contents)));
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    std::string read_contents;
    EXPECT_TRUE(base::ReadFileToString(test_file, &read_contents));
    EXPECT_EQ(file_contents, read_contents);
  }
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       SaveFile_TruncatesExistingFile) {
  const base::FilePath test_file = CreateTestFile("Hello World");

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showSaveFilePicker();"
                   "  self.entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_SAVEAS_FILE, dialog_params.type);
  EXPECT_EQ("",
            EvalJs(shell(),
                   "(async () => { const file = await self.entry.getFile(); "
                   "return await file.text(); })()"));
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       SaveFile_BlockedPermission) {
  const base::FilePath test_file = CreateTestFile("Save File");
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  EXPECT_CALL(permission_context,
              CanObtainReadPermission(url::Origin::Create(
                  embedded_test_server()->GetURL("/title1.html"))))
      .WillOnce(testing::Return(true));
  EXPECT_CALL(permission_context,
              CanObtainWritePermission(url::Origin::Create(
                  embedded_test_server()->GetURL("/title1.html"))))
      .WillOnce(testing::Return(false));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  auto result = EvalJs(shell(), "self.showSaveFilePicker()");
  EXPECT_TRUE(result.error.find("not allowed") != std::string::npos)
      << result.error;
  EXPECT_EQ(ui::SelectFileDialog::SELECT_NONE, dialog_params.type);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, FullscreenSaveFile) {
  const base::FilePath test_file = CreateTestFile("Hello World");

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EnterFullscreen();
  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showSaveFilePicker();"
                   "  self.entry = e;"
                   "  return e.name; })()"));
  EXPECT_FALSE(IsFullscreen());
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, OpenMultipleFiles) {
  const base::FilePath test_file1 = CreateTestFile("file1");
  const base::FilePath test_file2 = CreateTestFile("file2");
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(new FakeSelectFileDialogFactory(
      {test_file1, test_file2}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(ListValueOf(test_file1.BaseName().AsUTF8Unsafe(),
                        test_file2.BaseName().AsUTF8Unsafe()),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showOpenFilePicker("
                   "      {multiple: true});"
                   "  return e.map(x => x.name); })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE, dialog_params.type);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       FullscreenOpenMultipleFiles) {
  const base::FilePath test_file1 = CreateTestFile("file1");
  const base::FilePath test_file2 = CreateTestFile("file2");
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(new FakeSelectFileDialogFactory(
      {test_file1, test_file2}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EnterFullscreen();
  EXPECT_EQ(ListValueOf(test_file1.BaseName().AsUTF8Unsafe(),
                        test_file2.BaseName().AsUTF8Unsafe()),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showOpenFilePicker("
                   "      {multiple: true});"
                   "  return e.map(x => x.name); })()"));
  EXPECT_FALSE(IsFullscreen());
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, OpenDirectory) {
  base::FilePath test_dir = CreateTestDir();
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, FullscreenOpenDirectory) {
  base::FilePath test_dir = CreateTestDir();
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EnterFullscreen();
  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_FALSE(IsFullscreen());
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       OpenDirectory_BlockedPermission) {
  base::FilePath test_dir = CreateTestDir();
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  EXPECT_CALL(permission_context,
              CanObtainReadPermission(url::Origin::Create(
                  embedded_test_server()->GetURL("/title1.html"))))
      .WillOnce(testing::Return(false));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  auto result = EvalJs(shell(), "self.showDirectoryPicker()");
  EXPECT_TRUE(result.error.find("not allowed") != std::string::npos)
      << result.error;
  EXPECT_EQ(ui::SelectFileDialog::SELECT_NONE, dialog_params.type);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, OpenDirectory_DenyAccess) {
  base::FilePath test_dir = CreateTestDir();
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto read_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();
  auto write_grant = base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
      PermissionStatus::ASK, base::FilePath());

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());

  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));

  EXPECT_CALL(
      permission_context,
      GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory::kDefault))
      .WillOnce(testing::Return(base::FilePath()));
  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(PathInfo()));
  EXPECT_CALL(permission_context,
              SetLastPickedDirectory(origin, std::string(), test_dir,
                                     PathType::kLocal));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAllowed));

  EXPECT_CALL(permission_context,
              GetReadPermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(read_grant));
  EXPECT_CALL(permission_context,
              GetWritePermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(write_grant));

  EXPECT_CALL(
      *read_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserDenied));
  EXPECT_CALL(*read_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::ASK));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  auto result = EvalJs(shell(), "self.showDirectoryPicker()");
  EXPECT_TRUE(result.error.find("aborted") != std::string::npos)
      << result.error;
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       OpenDirectoryWithReadAccess) {
  base::FilePath test_dir = CreateTestDir();
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto read_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();
  auto write_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());

  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));

  EXPECT_CALL(
      permission_context,
      GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory::kDefault))
      .WillOnce(testing::Return(base::FilePath()));
  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(PathInfo()));
  EXPECT_CALL(permission_context,
              SetLastPickedDirectory(origin, std::string(), test_dir,
                                     PathType::kLocal));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAllowed));

  EXPECT_CALL(permission_context,
              GetReadPermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(read_grant));
  EXPECT_CALL(permission_context,
              GetWritePermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(write_grant));

  EXPECT_CALL(
      *read_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserGranted));
  EXPECT_CALL(*read_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::GRANTED));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker({mode: 'read'});"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       OpenDirectoryWithReadWriteAccess) {
  base::FilePath test_dir = CreateTestDir();
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto read_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();
  auto write_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());

  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));
  // Write permission should be requested alongside read permission.
  EXPECT_CALL(permission_context, CanObtainWritePermission(origin))
      .WillOnce(testing::Return(true));

  EXPECT_CALL(
      permission_context,
      GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory::kDefault))
      .WillOnce(testing::Return(base::FilePath()));
  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(PathInfo()));
  EXPECT_CALL(permission_context,
              SetLastPickedDirectory(origin, std::string(), test_dir,
                                     PathType::kLocal));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAllowed));

  EXPECT_CALL(permission_context,
              GetReadPermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(read_grant));
  EXPECT_CALL(permission_context,
              GetWritePermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(write_grant));

  EXPECT_CALL(
      *read_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserGranted));
  EXPECT_CALL(*read_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::GRANTED));
  // Write permission should be requested alongside read permission.
  EXPECT_CALL(
      *write_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserGranted));
  EXPECT_CALL(*write_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::GRANTED));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(
      test_dir.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(),
             "(async () => {"
             "  let e = await self.showDirectoryPicker({mode: 'readwrite'});"
             "  self.selected_entry = e;"
             "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       SaveFile_SensitiveDirectory_ExistingFile) {
  const std::string file_contents = "Hello World";
  const base::FilePath test_file = CreateTestFile(file_contents);

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());

  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));
  EXPECT_CALL(permission_context, CanObtainWritePermission(origin))
      .WillOnce(testing::Return(true));

  EXPECT_CALL(
      permission_context,
      GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory::kDefault))
      .WillOnce(testing::Return(base::FilePath()));
  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(PathInfo()));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_file,
                  FileSystemAccessPermissionContext::HandleType::kFile,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAbort));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  auto result = EvalJs(shell(), "self.showSaveFilePicker()");
  EXPECT_TRUE(result.error.find("aborted") != std::string::npos)
      << result.error;

  {
    // File should still exist, and be unmodified.
    base::ScopedAllowBlockingForTesting allow_blocking;
    std::string read_contents;
    EXPECT_TRUE(base::ReadFileToString(test_file, &read_contents));
    EXPECT_EQ(file_contents, read_contents);
  }
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       SaveFile_SensitiveDirectory_NonExistingFile) {
  const base::FilePath test_file = CreateTestFile("");
  {
    // Delete file, since SaveFile should be able to deal with non-existing
    // files.
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::DeleteFile(test_file));
  }

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());

  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));
  EXPECT_CALL(permission_context, CanObtainWritePermission(origin))
      .WillOnce(testing::Return(true));

  EXPECT_CALL(
      permission_context,
      GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory::kDefault))
      .WillOnce(testing::Return(base::FilePath()));
  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(PathInfo()));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_file,
                  FileSystemAccessPermissionContext::HandleType::kFile,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAbort));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  auto result = EvalJs(shell(), "self.showSaveFilePicker()");
  EXPECT_TRUE(result.error.find("aborted") != std::string::npos)
      << result.error;

  {
    // File should not have been created.
    base::ScopedAllowBlockingForTesting allow_blocking;
    EXPECT_FALSE(base::PathExists(test_file));
  }
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, AcceptsOptions) {
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new CancellingSelectFileDialogFactory(&dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  auto result =
      EvalJs(shell(),
             "self.showOpenFilePicker({types: ["
             "  {description: 'foo', accept: {'text/custom': ['.txt', '.Js']}},"
             "  {accept: {'image/jpeg': []}},"
             "  {accept: {'image/svg+xml': '.svg'}},"
             "]})");
  EXPECT_TRUE(result.error.find("aborted") != std::string::npos)
      << result.error;

  ASSERT_TRUE(dialog_params.file_types);
  EXPECT_TRUE(dialog_params.file_types->include_all_files);
  ASSERT_EQ(3u, dialog_params.file_types->extensions.size());
  ASSERT_EQ(2u, dialog_params.file_types->extensions[0].size());
  EXPECT_EQ(FILE_PATH_LITERAL("txt"),
            dialog_params.file_types->extensions[0][0]);
  EXPECT_EQ(FILE_PATH_LITERAL("Js"),
            dialog_params.file_types->extensions[0][1]);
  EXPECT_TRUE(base::Contains(dialog_params.file_types->extensions[1],
                             FILE_PATH_LITERAL("jpg")));
  EXPECT_TRUE(base::Contains(dialog_params.file_types->extensions[1],
                             FILE_PATH_LITERAL("jpeg")));
  EXPECT_TRUE(base::Contains(dialog_params.file_types->extensions[2],
                             FILE_PATH_LITERAL("svg")));

  ASSERT_EQ(3u,
            dialog_params.file_types->extension_description_overrides.size());
  EXPECT_EQ(u"foo",
            dialog_params.file_types->extension_description_overrides[0]);
  EXPECT_EQ(u"", dialog_params.file_types->extension_description_overrides[1]);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       FileSystemAccessUsageDisablesBackForwardCache) {
  BackForwardCacheDisabledTester tester;

  const base::FilePath test_file = CreateTestFile("file contents");
  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_TRUE(tester.IsDisabledForFrameWithReason(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID(),
      BackForwardCacheDisable::DisabledReason(
          BackForwardCacheDisable::DisabledReasonId::kFileSystemAccess)));
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       OpenDirectory_LastPickedDirExists) {
  base::FilePath test_dir = CreateTestDir();

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto read_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();
  auto write_grant = base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
      PermissionStatus::GRANTED, base::FilePath());

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());
  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));

  // The last picked directory exists.
  PathInfo good_dir_info;
  good_dir_info.path = temp_dir_.GetPath();

  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(good_dir_info));
  EXPECT_CALL(permission_context,
              SetLastPickedDirectory(origin, std::string(), test_dir,
                                     PathType::kLocal));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAllowed));

  EXPECT_CALL(permission_context,
              GetReadPermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(read_grant));
  EXPECT_CALL(permission_context,
              GetWritePermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(write_grant));

  EXPECT_CALL(
      *read_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserGranted));
  EXPECT_CALL(*read_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::GRANTED));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(good_dir_info.path, dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       OpenDirectory_LastPickedDirNotExists) {
  base::FilePath test_dir = CreateTestDir();

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto read_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();
  auto write_grant = base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
      PermissionStatus::GRANTED, base::FilePath());

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());
  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));

  // The last picked directory no longer exists, so resort to showing the
  // default directory, then set the test_file's dir as last picked.
  PathInfo bad_dir_info;
  bad_dir_info.path = temp_dir_.GetPath().AppendASCII("nonexistent");
  base::FilePath default_dir;
  default_dir = temp_dir_.GetPath().AppendASCII("default");

  EXPECT_CALL(
      permission_context,
      GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory::kDefault))
      .WillOnce(testing::Return(default_dir));
  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(bad_dir_info));
  EXPECT_CALL(permission_context,
              SetLastPickedDirectory(origin, std::string(), test_dir,
                                     PathType::kLocal));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAllowed));

  EXPECT_CALL(permission_context,
              GetReadPermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(read_grant));
  EXPECT_CALL(permission_context,
              GetWritePermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(write_grant));

  EXPECT_CALL(
      *read_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserGranted));
  EXPECT_CALL(*read_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::GRANTED));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(default_dir, dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       OpenDirectory_LastPickedDirExistsExternal) {
  base::FilePath test_dir = CreateTestDir();

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto read_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();
  auto write_grant = base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
      PermissionStatus::GRANTED, base::FilePath());

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());
  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));

  // The last picked directory exists.
  PathInfo good_dir_info;
  good_dir_info.path = base::FilePath::FromUTF8Unsafe(kTestMountPoint);
  good_dir_info.type = PathType::kExternal;

  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(good_dir_info));
  EXPECT_CALL(permission_context,
              SetLastPickedDirectory(origin, std::string(), test_dir,
                                     PathType::kLocal));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAllowed));

  EXPECT_CALL(permission_context,
              GetReadPermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(read_grant));
  EXPECT_CALL(permission_context,
              GetWritePermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(write_grant));

  EXPECT_CALL(
      *read_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserGranted));
  EXPECT_CALL(*read_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::GRANTED));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  // temp_dir_.GetPath() maps to kTestMountPoint.
  EXPECT_EQ(temp_dir_.GetPath(), dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       OpenDirectory_LastPickedDirNotExistsExternal) {
  base::FilePath test_dir = CreateTestDir();

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto read_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();
  auto write_grant = base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
      PermissionStatus::GRANTED, base::FilePath());

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());
  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));

  // The last picked directory no longer exists, so resort to showing the
  // default directory, then set the test_file's dir as last picked.
  PathInfo bad_dir_info;
  bad_dir_info.path = base::FilePath::FromUTF8Unsafe(kTestMountPoint)
                          .AppendASCII("nonexistent");
  base::FilePath default_dir;
  default_dir = temp_dir_.GetPath().AppendASCII("default");

  EXPECT_CALL(
      permission_context,
      GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory::kDefault))
      .WillOnce(testing::Return(default_dir));
  EXPECT_CALL(permission_context, GetLastPickedDirectory(origin, std::string()))
      .WillOnce(testing::Return(bad_dir_info));
  EXPECT_CALL(permission_context,
              SetLastPickedDirectory(origin, std::string(), test_dir,
                                     PathType::kLocal));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAllowed));

  EXPECT_CALL(permission_context,
              GetReadPermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(read_grant));
  EXPECT_CALL(permission_context,
              GetWritePermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(write_grant));

  EXPECT_CALL(
      *read_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserGranted));
  EXPECT_CALL(*read_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::GRANTED));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(default_dir, dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       StartIn_WellKnownDirectory) {
  base::FilePath test_dir = CreateTestDir();

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  testing::StrictMock<MockFileSystemAccessPermissionContext> permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  auto read_grant = base::MakeRefCounted<
      testing::StrictMock<MockFileSystemAccessPermissionGrant>>();
  auto write_grant = base::MakeRefCounted<FixedFileSystemAccessPermissionGrant>(
      PermissionStatus::GRANTED, base::FilePath());

  auto origin =
      url::Origin::Create(embedded_test_server()->GetURL("/title1.html"));
  auto frame_id = GlobalRenderFrameHostId(
      shell()->web_contents()->GetMainFrame()->GetProcess()->GetID(),
      shell()->web_contents()->GetMainFrame()->GetRoutingID());
  EXPECT_CALL(permission_context, CanObtainReadPermission(origin))
      .WillOnce(testing::Return(true));

  // Ensure Desktop directory exists.
  base::FilePath desktop_dir;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("Desktop"), &desktop_dir));
  }

  // Well-known starting directory specified, so do not call
  // GetLastPickedDirectory.
  EXPECT_CALL(
      permission_context,
      GetWellKnownDirectoryPath(blink::mojom::WellKnownDirectory::kDirDesktop))
      .WillOnce(testing::Return(desktop_dir));
  EXPECT_CALL(permission_context,
              SetLastPickedDirectory(origin, std::string(), test_dir,
                                     PathType::kLocal));

  EXPECT_CALL(permission_context,
              ConfirmSensitiveDirectoryAccess_(
                  origin, PathType::kLocal, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  frame_id, testing::_))
      .WillOnce(RunOnceCallback<5>(SensitiveDirectoryResult::kAllowed));

  EXPECT_CALL(permission_context,
              GetReadPermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(read_grant));
  EXPECT_CALL(permission_context,
              GetWritePermissionGrant(
                  origin, test_dir,
                  FileSystemAccessPermissionContext::HandleType::kDirectory,
                  FileSystemAccessPermissionContext::UserAction::kOpen))
      .WillOnce(testing::Return(write_grant));

  EXPECT_CALL(
      *read_grant,
      RequestPermission_(
          frame_id,
          FileSystemAccessPermissionGrant::UserActivationState::kNotRequired,
          testing::_))
      .WillOnce(RunOnceCallback<2>(FileSystemAccessPermissionGrant::
                                       PermissionRequestOutcome::kUserGranted));
  EXPECT_CALL(*read_grant, GetStatus())
      .WillRepeatedly(testing::Return(PermissionStatus::GRANTED));

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  EXPECT_EQ(
      test_dir.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(),
             "(async () => {"
             "  let e = await self.showDirectoryPicker({ startIn: 'desktop' });"
             "  self.selected_entry = e;"
             "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(desktop_dir, dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, StartIn_FileHandle) {
  // Ensure test file exists in a directory which could not be a default.
  base::FilePath test_file_dir;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("handles"), &test_file_dir));
  }
  const base::FilePath test_file =
      test_file_dir.Append(base::FilePath::FromUTF8Unsafe("file.txt"));

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  // Acquire a FileSystemHandle to the test_file.
  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_OPEN_FILE, dialog_params.type);
  EXPECT_EQ(shell()->web_contents()->GetTopLevelNativeWindow(),
            dialog_params.owning_window);

  EXPECT_EQ(test_file.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker({ startIn: "
                   "              self.selected_entry });"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_OPEN_FILE, dialog_params.type);
  EXPECT_EQ(test_file_dir, dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, StartIn_DirectoryHandle) {
  // Ensure test directory exists and could not be a default.
  base::FilePath test_dir;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("handles"), &test_dir));
  }

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  // Acquire a FileSystemHandle to the test_dir.
  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(shell()->web_contents()->GetTopLevelNativeWindow(),
            dialog_params.owning_window);

  EXPECT_EQ(test_dir.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let e = await self.showDirectoryPicker({ startIn: "
                   "              self.selected_entry });"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(test_dir, dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       StartIn_FileHandle_External) {
  const base::FilePath test_file = CreateTestFile("");
  const base::FilePath virtual_path =
      base::FilePath::FromUTF8Unsafe(kTestMountPoint)
          .Append(test_file.BaseName());

  ui::SelectedFileInfo selected_file = {base::FilePath(), base::FilePath()};
  selected_file.virtual_path = virtual_path;

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({selected_file}, &dialog_params));
  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  // Acquire a FileSystemHandle to the test_file.
  EXPECT_EQ(virtual_path.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker();"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_OPEN_FILE, dialog_params.type);
  EXPECT_EQ(shell()->web_contents()->GetTopLevelNativeWindow(),
            dialog_params.owning_window);

  EXPECT_EQ(virtual_path.BaseName().AsUTF8Unsafe(),
            EvalJs(shell(),
                   "(async () => {"
                   "  let [e] = await self.showOpenFilePicker({ startIn: "
                   "              self.selected_entry });"
                   "  self.selected_entry = e;"
                   "  return e.name; })()"));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_OPEN_FILE, dialog_params.type);
  // temp_dir_.GetPath() maps to kTestMountPoint.
  EXPECT_EQ(temp_dir_.GetPath(), dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, SuggestedName) {
  const base::FilePath test_file = CreateTestFile("");
  SelectFileDialogParams dialog_params;

  struct info {
    std::string suggested_name;
    base::ListValue accepted_extensions;
    bool exclude_accept_all_option = true;
    std::string expected_result;
    bool expected_exclude_accept_all_option = false;
  };

  std::vector<info> name_infos;
  // Empty suggested name should be ok.
  name_infos.push_back({"", ListValueOf(".txt"), true, "", true});
  name_infos.push_back({"", ListValueOf(".txt"), false, "", false});
  name_infos.push_back({"", ListValueOf(), true, "", false});

  // Suggested extension listed as accepted extension.
  name_infos.push_back(
      {"ext_match.txt", ListValueOf(".txt"), true, "ext_match.txt", true});
  name_infos.push_back(
      {"ext_match.txt", ListValueOf(".txt"), false, "ext_match.txt", false});
  name_infos.push_back(
      {"ext_match.txt", ListValueOf(), true, "ext_match.txt", false});

  // No suggested extension. Don't try to infer one, and behave as if
  // |excludeAcceptAllOption| is false.
  name_infos.push_back(
      {"no_extension", ListValueOf(".txt"), true, "no_extension", false});
  name_infos.push_back(
      {"no_extension", ListValueOf(".txt"), false, "no_extension", false});
  name_infos.push_back(
      {"no_extension", ListValueOf(), true, "no_extension", false});

  // Suggested extension not listed as an accepted extension. Allow extension,
  // but behave as if |excludeAcceptAllOption| is false.
  name_infos.push_back({"not_matching.jpg", ListValueOf(".txt"), true,
                        "not_matching.jpg", false});
  name_infos.push_back({"not_matching.jpg", ListValueOf(".txt"), false,
                        "not_matching.jpg", false});

#if defined(OS_WIN)
  // ".local" and ".lnk" extensions should be sanitized on Windows.
  name_infos.push_back({"dangerous_extension.local", ListValueOf(".local"),
                        true, "dangerous_extension.download", false});
  name_infos.push_back({"dangerous_extension.lnk", ListValueOf(".lnk"), true,
                        "dangerous_extension.download", false});
#else
  // ".local" and ".lnk" extensions should be allowed on other OSes.
  // TODO(https://crbug.com/1154757): `expected_exclude_accept_all_option` is
  // false here because ".local" and ".lnk" extensions are not allowed in
  // `accepts`, but are only sanitized by net::GenerateSafeFileName on Windows.
  name_infos.push_back({"dangerous_extension.local", ListValueOf(".local"),
                        true, "dangerous_extension.local", false});
  name_infos.push_back({"dangerous_extension.lnk", ListValueOf(".lnk"), true,
                        "dangerous_extension.lnk", false});
#endif
  // Invalid characters should be sanitized.
  name_infos.push_back({R"(inv*l:d\\charבאמת!a<ters🤓.txt)",
                        ListValueOf(".txt"), true,
                        R"(inv_l_d__charבאמת!a_ters🤓.txt)", true});

  for (const auto& name_info : name_infos) {
    SCOPED_TRACE(name_info.suggested_name);
    ui::SelectFileDialog::SetFactory(
        new FakeSelectFileDialogFactory({test_file}, &dialog_params));
    ASSERT_TRUE(
        NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
    EXPECT_EQ(
        test_file.BaseName().AsUTF8Unsafe(),
        EvalJs(shell(), JsReplace("(async () => {"
                                  "  let e = await self.showSaveFilePicker({"
                                  "    suggestedName: $1,"
                                  "    types: [{accept: {'text/custom': $2}}],"
                                  "    excludeAcceptAllOption: $3"
                                  "});"
                                  "  return e.name; })()",
                                  name_info.suggested_name,
                                  name_info.accepted_extensions,
                                  name_info.exclude_accept_all_option)));
    EXPECT_EQ(ui::SelectFileDialog::SELECT_SAVEAS_FILE, dialog_params.type);
    EXPECT_EQ(base::FilePath::FromUTF8Unsafe(name_info.expected_result),
              dialog_params.default_path.BaseName());
    EXPECT_NE(name_info.expected_exclude_accept_all_option,
              dialog_params.file_types->include_all_files);
  }
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest,
                       SuggestedName_CorrectIndex) {
  const base::FilePath test_file = CreateTestFile("");
  SelectFileDialogParams dialog_params;

  struct info {
    std::string suggested_name;
    std::string expected_result;
    bool expected_exclude_accept_all_option = false;
    int expected_index;
  };

  std::vector<info> name_infos;
  // There are valid accepted extensions, so default to index 1.
  name_infos.push_back({"ext_no_match.foo", "ext_no_match.foo", false, 1});
  name_infos.push_back({"ext_match.jpg", "ext_match.jpg", true, 1});
  name_infos.push_back({"ext_match.txt", "ext_match.txt", true, 2});
  name_infos.push_back({"ext_mime_match.text", "ext_mime_match.text", true, 2});

  for (const auto& name_info : name_infos) {
    SCOPED_TRACE(name_info.suggested_name);
    ui::SelectFileDialog::SetFactory(
        new FakeSelectFileDialogFactory({test_file}, &dialog_params));
    ASSERT_TRUE(
        NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
    EXPECT_EQ(
        test_file.BaseName().AsUTF8Unsafe(),
        EvalJs(shell(), JsReplace("(async () => {"
                                  "  let e = await self.showSaveFilePicker({"
                                  "    suggestedName: $1,"
                                  "    types: ["
                                  "     {accept: {'image/custom': ['.jpg']}},"
                                  "     {accept: {'text/plain': ['.txt']}},"
                                  "    ],"
                                  "    excludeAcceptAllOption: true"
                                  "});"
                                  "  return e.name; })()",
                                  name_info.suggested_name)));
    EXPECT_EQ(ui::SelectFileDialog::SELECT_SAVEAS_FILE, dialog_params.type);
    EXPECT_EQ(base::FilePath::FromUTF8Unsafe(name_info.expected_result),
              dialog_params.default_path.BaseName());
    EXPECT_NE(name_info.expected_exclude_accept_all_option,
              dialog_params.file_types->include_all_files);
    EXPECT_EQ(name_info.expected_index, dialog_params.file_type_index);
  }
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, StartIn_ID) {
  base::FilePath test_dir;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    // Ensure directory we're selecting exists.
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("test123"), &test_dir));
  }

  SelectFileDialogParams dialog_params;
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({test_dir}, &dialog_params));

  FakeFileSystemAccessPermissionContext permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  // Specify an `id` for the directory that is picked.
  std::string id = "testing";
  PathInfo test_dir_info;
  test_dir_info.path = test_dir;

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));
  // #1: `id` is unset. Fall back to the default starting directory.
  EXPECT_EQ(
      test_dir.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(),
             JsReplace("(async () => {"
                       "  let e = await self.showDirectoryPicker({ id: $1 });"
                       "  self.selected_entry = e;"
                       "  return e.name; })()",
                       id)));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(base::FilePath(), dialog_params.default_path);

  // #2: `id` is set. Use the LastPickedDirectory given this `id`.
  EXPECT_EQ(
      test_dir.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(),
             JsReplace("(async () => {"
                       "  let e = await self.showDirectoryPicker({ id: $1 });"
                       "  self.selected_entry = e;"
                       "  return e.name; })()",
                       id)));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(test_dir, dialog_params.default_path);
}

IN_PROC_BROWSER_TEST_F(FileSystemChooserBrowserTest, StartIn_Priority) {
  // Priority:
  //   1) `startIn` via a file/directory handle
  //   2) non-empty `id, if stored
  //   3) `startIn` via a well-known directory
  //   4) default `id`, if stored
  //   5) default path
  //
  // Test A checks #5
  //      B checks #4
  //      C checks #3
  //      D checks #2
  //      E checks #1

  base::FilePath test_dir;
  base::FilePath desktop_dir;
  base::FilePath music_dir;
  base::FilePath dir_handle;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    // Ensure directories we're selecting exist.
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("test123"), &test_dir));
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("Desktop"), &desktop_dir));
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("Music"), &music_dir));
    EXPECT_TRUE(base::CreateTemporaryDirInDir(
        temp_dir_.GetPath(), FILE_PATH_LITERAL("handle"), &dir_handle));
  }

  FakeFileSystemAccessPermissionContext permission_context;
  static_cast<FileSystemAccessManagerImpl*>(
      shell()
          ->web_contents()
          ->GetBrowserContext()
          ->GetStoragePartition(shell()->web_contents()->GetSiteInstance())
          ->GetFileSystemAccessEntryFactory())
      ->SetPermissionContextForTesting(&permission_context);

  permission_context.SetWellKnownDirectoryPath(
      blink::mojom::WellKnownDirectory::kDirDesktop, desktop_dir);
  permission_context.SetWellKnownDirectoryPath(
      blink::mojom::WellKnownDirectory::kDirMusic, music_dir);

  // Specify an `id` for the directory that is picked.
  std::string id = "testing";
  PathInfo test_dir_info;
  test_dir_info.path = test_dir;
  PathInfo desktop_dir_info;
  desktop_dir_info.path = desktop_dir;

  ASSERT_TRUE(
      NavigateToURL(shell(), embedded_test_server()->GetURL("/title1.html")));

  SelectFileDialogParams dialog_params;
  // (A) Acquire a handle to the "handle" directory to be used later. Use the
  // default `id`.
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({dir_handle}, &dialog_params));
  EXPECT_EQ(
      dir_handle.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(), JsReplace("(async () => {"
                                "  let e = await self.showDirectoryPicker();"
                                "  self.handle = e;"
                                "  return e.name; })()",
                                id)));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(base::FilePath(), dialog_params.default_path);

  // (B) Use the default `id`, which should have been set.
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({dir_handle}, &dialog_params));
  EXPECT_EQ(
      dir_handle.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(), JsReplace("(async () => {"
                                "  let e = await self.showDirectoryPicker();"
                                "  self.handle = e;"
                                "  return e.name; })()",
                                id)));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(dir_handle, dialog_params.default_path);

  // (C) Since this new `id` has not yet been set, fall back on using the
  // WellKnownDirectory specified via `startIn`.
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({desktop_dir}, &dialog_params));
  EXPECT_EQ(
      desktop_dir.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(), JsReplace("(async () => {"
                                "  let e = await self.showDirectoryPicker({ "
                                "id: $1, startIn: 'desktop' });"
                                "  self.selected_entry = e;"
                                "  return e.name; })()",
                                id)));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(desktop_dir, dialog_params.default_path);

  // (D) The `id` is set. Use the LastPickedDirectory given this `id`.
  EXPECT_EQ(
      desktop_dir.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(), JsReplace("(async () => {"
                                "  let e = await self.showDirectoryPicker({ "
                                "id: $1, startIn: 'music' });"
                                "  self.selected_entry = e;"
                                "  return e.name; })()",
                                id)));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(desktop_dir, dialog_params.default_path);

  // (E) A directory handle is specified via `startIn`, so prioritize this over
  // the stored ID.
  ui::SelectFileDialog::SetFactory(
      new FakeSelectFileDialogFactory({dir_handle}, &dialog_params));
  EXPECT_EQ(
      dir_handle.BaseName().AsUTF8Unsafe(),
      EvalJs(shell(), JsReplace("(async () => {"
                                "  let e = await self.showDirectoryPicker({ "
                                "id: $1, startIn: self.handle });"
                                "  self.selected_entry = e;"
                                "  return e.name; })()",
                                id)));
  EXPECT_EQ(ui::SelectFileDialog::SELECT_FOLDER, dialog_params.type);
  EXPECT_EQ(dir_handle, dialog_params.default_path);
}

}  // namespace content
