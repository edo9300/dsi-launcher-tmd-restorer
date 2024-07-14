#include <cstdint>
#include <dirent.h>
#include <format>
#include <memory>
#include <print>
#include <string_view>

#include "main.h"
#include "message.h"
#include "nand/nandio.h"
#include "storage.h"
#include "version.h"
#include "nitrofs.h"
#include "deviceList.h"
#include "sha1digest.h"

volatile bool programEnd = false;
static volatile bool arm7Exiting = false;
volatile bool charging = false;
volatile u8 batteryLevel = 0;

PrintConsole topScreen;
PrintConsole bottomScreen;

static void setupScreens()
{
	REG_DISPCNT = MODE_FB0;
	VRAM_A_CR = VRAM_ENABLE;

	videoSetMode(MODE_0_2D);
	videoSetModeSub(MODE_0_2D);

	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);

	consoleInit(&topScreen, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
	consoleInit(&bottomScreen, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

	clearScreen(&bottomScreen);

	VRAM_A[100] = 0xFFFF;
}

static void cleanup()
{
	programEnd = true;
	clearScreen(&bottomScreen);
	std::println("Unmounting NAND...");
	fatUnmount("nand:");
	std::println("Merging stages...");
	nandio_shutdown();

	fifoSendValue32(FIFO_USER_02, 0x54495845); // 'EXIT'

	while (arm7Exiting)
		swiWaitForVBlank();
}

[[noreturn]] static void exitWithMessage(std::string_view message)
{
	messageBox(message.data());
	cleanup();
	std::exit(0);
}

[[noreturn]] static void abortWithError(std::string_view error)
{
	exitWithMessage(std::format("\x1B[31mError:\x1B[33m {}", error));
}

static auto getSourceAndTargetTmds() {
	auto launcher_tmd_str = [] -> std::string {
		uint32_t launcherTid;
		{
			auto* file = fopen("nand:/sys/HWINFO_S.dat", "rb");
			if(!file)
				abortWithError("Could not open HWINFO_S.dat");
			fseek(file, 0xA0, SEEK_SET);
			fread(&launcherTid, sizeof(uint32_t), 1, file);
			fclose(file);
		}
		return std::format("{:08x}", launcherTid);
	}();

	auto launcher_content_path = std::format("nand:/title/00030017/{}/content", launcher_tmd_str);

	auto [expected_launcher_version, launcher_app_name] = [&] {
		std::shared_ptr<DIR> pdir{opendir(launcher_content_path.c_str()), closedir};
		if (!pdir)
			abortWithError(std::format("Could not open launcher title directory ({})", launcher_content_path));
		dirent* pent;
		while((pent = readdir(pdir.get())) != nullptr) {
			if(pent->d_type == DT_DIR)
				continue;
			std::string_view filename{pent->d_name};
			if(filename.size() != 12 || !filename.ends_with(".app") || !filename.starts_with("0000000"))
				continue;
			auto launcher_app_version = static_cast<uint16_t>(static_cast<unsigned char>(filename[7]) - static_cast<unsigned char>('0'));
			if(launcher_app_version > 7)
				abortWithError(std::format("Found an unsupported launcher version: {}", launcher_app_version));
			return std::pair{256 * launcher_app_version, std::string{filename}};
		}
		abortWithError("Launcher app not found");
	}();

	return std::tuple{std::format("nitro:/{}/tmd.{}", launcher_tmd_str, static_cast<int>(expected_launcher_version)),
					  std::format("{}/title.tmd", launcher_content_path),
					  std::format("{}/{}", launcher_content_path, launcher_app_name)};
}

auto checkTmdAndReadBuffer(auto sourceTmdPath, auto targetTmdPath)
{
	auto expectedSha1Tmd = [&] -> Sha1Digest {
		auto file = fopen(std::format("{}.sha1", sourceTmdPath).data(), "rb");
		if(!file)
			abortWithError("Tmd sha1 not found");
		char sha1StrBuff[41]{};
		auto read = fread(sha1StrBuff, sizeof(sha1StrBuff) - 1, 1, file);
		fclose(file);
		if(read != 1)
			abortWithError("Failed to parse good tmd's sha1 file");
		return {sha1StrBuff};
	}();

	auto actualSha1Tmd = [&] -> Sha1Digest {
		Sha1Digest ret;
		auto* targetTmd = fopen(targetTmdPath.data(), "rb");
		if(!targetTmd)
			abortWithError(std::format("Failed to open target tmd ({})", targetTmdPath));

		if(!calculateFileSha1(targetTmd, ret.data())) {
			fclose(targetTmd);
			abortWithError("Failed to parse tmd's sha1 file");
		}
		fclose(targetTmd);

		return ret;
	}();

	auto sourceTmdBuffer = [&] {
		auto* sourceTmd = fopen(sourceTmdPath.data(), "rb");
		if(!sourceTmd)
			abortWithError(std::format("Failed to open source tmd ({})", sourceTmdPath));

		std::array<uint8_t, 520>  ret;
		auto read = fread(ret.data(), ret.size(), 1, sourceTmd);
		fclose(sourceTmd);
		if(read != 1)
			abortWithError(std::format("Failed to read source tmd ({})", sourceTmdPath));
		Sha1Digest digest;
		swiSHA1Calc(digest.data(), ret.data(), ret.size());
		if(digest != expectedSha1Tmd)
			abortWithError(std::format("Source tmd's hash doesn't match ({})", sourceTmdPath));
		return ret;
	}();

	if(expectedSha1Tmd == actualSha1Tmd)
	{
		exitWithMessage("The tmd is correct, no further action needed");
	}

	return sourceTmdBuffer;
}

int main(int argc, char **argv)
{
	keysSetRepeat(25, 5);
	setupScreens();

	fifoSetValue32Handler(FIFO_USER_01, [](u32 value32, void* userdata){
		if (value32 == 0x54495845) // 'EXIT'
		{
			programEnd = true;
			arm7Exiting = true;
		}
	}, nullptr);

	fifoSetValue32Handler(FIFO_USER_03, [](u32 value32, void* userdata){
		batteryLevel = value32 & 0xF;
		charging = (value32 & BIT(7)) != 0;
	}, nullptr);

	//DSi check
	if (!isDSiMode())
	{
		abortWithError("This app is exclusively for DSi.");
		return 0;
	}

	if (!fatInitDefault())
		abortWithError("fatInitDefault()...\x1B[31mFailed\n\x1B[47m");

	//setup nand access
	if (!fatMountSimple("nand", &io_dsi_nand))
		abortWithError("nand init \x1B[31mfailed\n\x1B[47m");

	while (batteryLevel < 7 && !charging)
	{
		if (choiceBox("\x1B[47mBattery is too low!\nPlease plug in the console.\n\nContinue?") == NO)
			return 0;
	}

	DeviceList* deviceList = getDeviceList();

	auto applicationPath = [&] -> const char* {
		if(argc > 0)
			return argv[0];
		if(deviceList)
			return deviceList->appname;
		return "sd:/ntrboot.nds";
	}();

	if (!nitroFSInit(applicationPath))
	{
		abortWithError("nitroFSInit()...\x1B[31mFailed\n\x1B[47m");
	}

	clearScreen(&topScreen);

	auto [sourceTmdPath, targetTmdPath, launcherAppPath] = getSourceAndTargetTmds();

	clearScreen(&topScreen);
	std::println("\tLauncher tmd restorer");
	std::println("\nversion {}", VERSION);
	std::println("\nedo9300 - 2024");
	std::print("\x1b[10;0HDetected launcher version: v{}", sourceTmdPath.substr(20));
	std::print("\x1b[11;0HDetected launcher region: {}", [&] {
		if(auto launcherTidString = sourceTmdPath.substr(13,2); launcherTidString == "43")
			return "C";
		else if(launcherTidString == "45")
			return "U";
		else if(launcherTidString == "4a")
			return "J";
		else if(launcherTidString == "4b")
			return "K";
		else if(launcherTidString == "50")
			return "E";
		else if(launcherTidString == "55")
			return "A";
		return "UNK";
	}());
	

	messageBox("\x1B[41mWARNING:\x1B[47m This tool can write to\n"
				"your internal NAND!\n\n"
				"This always has a risk, albeit\n"
				"low, of \x1B[41mbricking\x1B[47m your system\n"
				"and should be done with caution!\n\n"
				"If you have not yet done so,\n"
				"you should make a NAND backup.");
				
	auto correctTmdBuffer = checkTmdAndReadBuffer(sourceTmdPath, targetTmdPath);

	if(choiceBox("Do you want to restore\n"
				 "the launcher's tmd?") == NO)
		exitWithMessage("Aborted");

	if(!nandio_unlock_writing())
		abortWithError("Failed to mount the nand as writable");

	// Unlaunch might've left us a nice gift
	if(!toggleFileReadOnly(targetTmdPath.data(), false))
		abortWithError("Failed to mark target tmd as writable");
	if(!toggleFileReadOnly(launcherAppPath.data(), false))
		abortWithError("Failed to mark launcher app as writable");

	auto targetTmd = fopen(targetTmdPath.data(), "r+b");

	if (ftruncate(fileno(targetTmd), 520) != 0) {
		fclose(targetTmd);
		abortWithError("Failed to truncate target tmd as right size");
	}
	fseek(targetTmd, 0, SEEK_SET);
	auto written = fwrite(correctTmdBuffer.data(), correctTmdBuffer.size(), 1, targetTmd);
	fclose(targetTmd);
	if(written != 1)
		abortWithError("Faied to write tmd");

	exitWithMessage("Done");
}

void clearScreen(PrintConsole* screen)
{
	consoleSelect(screen);
	consoleClear();
}