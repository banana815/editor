import { check, type DownloadEvent } from "@tauri-apps/plugin-updater";

export type InstallUpdateResult =
  | {
      installed: false;
      currentVersion: string;
    }
  | {
      installed: true;
      currentVersion: string;
      latestVersion: string;
      downloadedBytes: number;
      contentLength: number;
    };

export async function installLatestUpdate(
  onProgress?: (progress: { downloadedBytes: number; contentLength: number }) => void
): Promise<InstallUpdateResult> {
  const update = await check({ timeout: 30000 });

  if (!update) {
    return {
      installed: false,
      currentVersion: "",
    };
  }

  let downloadedBytes = 0;
  let contentLength = 0;

  await update.downloadAndInstall((event: DownloadEvent) => {
    if (event.event === "Started") {
      contentLength = event.data.contentLength ?? 0;
      onProgress?.({ downloadedBytes, contentLength });
      return;
    }

    if (event.event === "Progress") {
      downloadedBytes += event.data.chunkLength;
      onProgress?.({ downloadedBytes, contentLength });
    }
  });

  return {
    installed: true,
    currentVersion: update.currentVersion,
    latestVersion: update.version,
    downloadedBytes,
    contentLength,
  };
}
