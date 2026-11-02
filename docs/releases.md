# Create a release

Releases are created manually. The Build workflow tests and packages the runtimes and Receiver; it does not publish releases.

1. Create and push a version tag, such as `v1.0.0`, on the commit to release. Runtime packages record the tag for the updater.
2. Wait for **all jobs** in that tag's Build workflow run to pass.
3. Download these artifacts from the run:
   - `SRanipatch.VRCFT.win-x64`
   - `SRanipatch-Runtime.win-x64`
   - `SRanipatch-Runtime.linux-arm64`
4. Extract the GitHub Actions artifact wrappers. Attach the **files inside** to a GitHub release for the same tag:
   - `SRanipatch.VRCFT.win-x64.zip`
   - `SRanipatch-Runtime.win-x64.zip`
   - `SRanipatch-Runtime.win-x64.zip.sha256`
   - `SRanipatch-Runtime.linux-arm64.tar.gz`
   - `SRanipatch-Runtime.linux-arm64.tar.gz.sha256`
5. Link to the [setup guide](../README.md#setup), describe the release's changes, and publish a stable release for the same tag. The bootstrap and updaters use GitHub's latest stable release.

Keep filenames unchanged. Both runtime archives and their checksums are required for installation and updates.
