/*
 * setup.c -- `hull setup windows ISO DIR` (section 5.0's qemu-pc): a
 * machine directory that installs Windows from the user's own disc with no
 * one at it, and then runs what it installed with an OpenSSH server the
 * user's keys open.
 *
 * What it writes, and why each part is there:
 *
 *   answer.img      a USB stick, FAT, holding autounattend.xml, which
 *                   Windows Setup looks for on removable media -- a CD of
 *                   hull's own is ISO9660 level 1 and has no name that long
 *                   -- and hull-setup.ps1 and authorized_keys, which the
 *                   first logon runs and reads;
 *   disk.qcow2      the disk Windows is installed to, 64 GiB, sparse;
 *   install.conf    the machine that installs: the disc booted, Enter
 *                   pressed for its "Press any key to boot from CD", the
 *                   stick beside it; it powers off when the first logon's
 *                   script is done;
 *   machine.conf    the machine that runs it, from its disk, port 22 at
 *                   2222 of its address;
 *   --drivers       the user's virtio-win disc, whose display files go on
 *                   the stick: the first logon installs Red Hat's virtio-gpu
 *                   display driver, viogpudo, and registers vgpusrv, the
 *                   service setting the mode the host asks for, and
 *                   machine.conf is on virtio-vga -- so the desktop follows
 *                   hull-gui's window, once somebody has logged on, which
 *                   Windows' Basic Display Adapter on qemu's standard VGA
 *                   cannot. The install itself is on the standard VGA, and
 *                   the driver waits in the store for the device: with the
 *                   disc as a second CD, three installs of four hung for
 *                   good at one of Setup's restarts, two of them on
 *                   virtio-vga and one not (project.md section 5.0);
 *   password        the account's, which the answer file must carry for
 *                   its automatic logon, 0600. The account logs on at the
 *                   console at every boot, as hull-gui's window wants: the
 *                   first logon's script sets it, the password in Winlogon.
 *
 * Both specs ask what Windows 11 asks of a PC (measured by fmake under pc,
 * 2026-11-12): q35, the host's CPU, a TPM 2.0, Secure Boot with Microsoft's
 * keys, the variables and the TPM kept in DIR, and the e1000e Windows has a
 * driver for. Windows 10 takes the same. The OpenSSH Server feature is
 * downloaded by Windows itself, so the install wants the network; its
 * firewall rule admits the Private profile alone and qemu's network is
 * Public, so the script opens it to every profile.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "setup.h"

/* `argv`, its output to /dev/null: 0 where it ran and exited 0. */
static int run(char *const argv[])
{
	pid_t child = fork();
	int   wstatus = 0;

	if (child == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0)
			_exit(126);
		execvp(argv[0], argv);
		_exit(127);
	}
	return child > 0 && waitpid(child, &wstatus, 0) == child && WIFEXITED(wstatus) &&
	       WEXITSTATUS(wstatus) == 0 ? 0 : -1;
}

/* `argv`, its output to /dev/null: its exit status, or -1 where it did
 * not exit. */
static int run_code(char *const argv[])
{
	pid_t child = fork();
	int   wstatus = 0;

	if (child == 0) {
		int null = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null < 0 || dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0)
			_exit(126);
		execvp(argv[0], argv);
		_exit(127);
	}
	return child > 0 && waitpid(child, &wstatus, 0) == child && WIFEXITED(wstatus) ?
	       WEXITSTATUS(wstatus) : -1;
}

/* `text` into DIR/NAME with `mode`: 0, or -1 with `why`. */
static int put(const char *dir, const char *name, const char *text, mode_t mode, char *why,
               size_t cap)
{
	char  path[PATH_MAX];
	int   fd;
	FILE *f;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode)) < 0 ||
	    (f = fdopen(fd, "w")) == NULL) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	fputs(text, f);
	if (fclose(f) != 0) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

/* The user's public keys, each .pub file in ~/.ssh, as the core gathers them for (ssh
 * true): allocated, or NULL where there are none. */
static char *public_keys(void)
{
	const char    *home = getenv("HOME");
	char           dir[PATH_MAX];
	DIR           *d;
	struct dirent *e;
	char          *keys = NULL;
	size_t         length = 0u;

	if (home == NULL || home[0] != '/')
		return NULL;
	snprintf(dir, sizeof dir, "%s/.ssh", home);
	if ((d = opendir(dir)) == NULL)
		return NULL;
	while ((e = readdir(d)) != NULL) {
		size_t n = strlen(e->d_name);
		char   path[PATH_MAX + 256];
		char   line[16384];
		FILE  *f;

		if (n < 5u || strcmp(e->d_name + n - 4u, ".pub") != 0)
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		if ((f = fopen(path, "re")) == NULL)
			continue;
		while (fgets(line, sizeof line, f) != NULL) {
			size_t add = strlen(line);
			char  *grown;

			if (strncmp(line, "ssh-", 4u) != 0 && strncmp(line, "ecdsa-", 6u) != 0 &&
			    strncmp(line, "sk-", 3u) != 0)
				continue;
			if ((grown = realloc(keys, length + add + 2u)) == NULL)
				break;
			keys = grown;
			memcpy(keys + length, line, add);
			length += add;
			if (keys[length - 1u] != '\n')
				keys[length++] = '\n';
			keys[length] = '\0';
		}
		fclose(f);
	}
	closedir(d);
	return keys;
}

/* A password of 24 letters and digits, from the kernel's random. */
static int password(char *out)
{
	static const char alphabet[] = "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
	unsigned char     bytes[24];
	int               fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	if (fd < 0 || read(fd, bytes, sizeof bytes) != (ssize_t)sizeof bytes) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	for (size_t i = 0u; i < sizeof bytes; i++)
		out[i] = alphabet[bytes[i] % (sizeof alphabet - 1u)];
	out[sizeof bytes] = '\0';
	return 0;
}

static const char script[] =
    "$ErrorActionPreference = 'Continue'\r\n"
    "Start-Transcript -Path C:\\hull-setup.log\r\n"
    "$here = Split-Path -Parent $MyInvocation.MyCommand.Path\r\n"
    "Get-WindowsCapability -Online -Name OpenSSH.Server* | Add-WindowsCapability -Online\r\n"
    "Set-Service -Name sshd -StartupType Automatic\r\n"
    "Start-Service sshd\r\n"
    "# The feature's rule admits the Private profile alone, and qemu's network\r\n"
    "# is classed Public: every connection would hang at the banner.\r\n"
    "if (Get-NetFirewallRule -Name OpenSSH-Server-In-TCP -ErrorAction SilentlyContinue) {\r\n"
    "  Set-NetFirewallRule -Name OpenSSH-Server-In-TCP -Profile Any\r\n"
    "} else {\r\n"
    "  New-NetFirewallRule -Name OpenSSH-Server-In-TCP -DisplayName 'OpenSSH Server' "
    "-Enabled True -Direction Inbound -Protocol TCP -Action Allow -LocalPort 22 -Profile Any\r\n"
    "}\r\n"
    "$keys = 'C:\\ProgramData\\ssh\\administrators_authorized_keys'\r\n"
    "Copy-Item (Join-Path $here 'authorized_keys') $keys\r\n"
    "icacls $keys /inheritance:r /grant 'Administrators:F' /grant 'SYSTEM:F'\r\n"
    "Stop-Transcript\r\n"
    "Stop-Computer -Force\r\n";

/* Before the script's end, the account logging on at the console at every
 * boot from now on, not only the answer file's one time: a machine at
 * hull-gui's window shows a desktop rather than a sign-in, and
 * virtio-win's resolution service sets a mode only in a logged-on session.
 * The answer's AutoLogon counts down to nothing, so its count goes; the
 * password is Winlogon's DefaultPassword, as plain as the password file
 * beside the spec. Format: the account, the password. */
static const char script_logon[] =
    "$logon = 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon'\r\n"
    "Set-ItemProperty $logon AutoAdminLogon '1'\r\n"
    "Set-ItemProperty $logon DefaultUserName '%s'\r\n"
    "Set-ItemProperty $logon DefaultPassword '%s'\r\n"
    "Remove-ItemProperty $logon AutoLogonCount -ErrorAction SilentlyContinue\r\n";

/* With --drivers, before the script's end: the display driver from
 * whichever drive holds viogpudo -- the stick -- for this Windows' release,
 * and the resolution service copied off it -- it registers its own path
 * -- and registered, starting with every boot. Beside it viogpuap, which
 * the service starts by bare name in a logged-on user's session and which
 * sets the mode the driver signals: without it nothing resizes. The driver
 * is signed by Microsoft's hardware programme, so Secure Boot takes it
 * unasked. */
static const char script_drivers[] =
    "$os = if ([Environment]::OSVersion.Version.Build -ge 22000) { 'w11' } else { 'w10' }\r\n"
    "$gpu = Get-PSDrive -PSProvider FileSystem | ForEach-Object { Join-Path $_.Root \"viogpudo\\$os\\amd64\" } |\r\n"
    "  Where-Object { Test-Path (Join-Path $_ 'viogpudo.inf') } | Select-Object -First 1\r\n"
    "if ($gpu) {\r\n"
    "  pnputil /add-driver (Join-Path $gpu 'viogpudo.inf') /install\r\n"
    "  $bin = Join-Path $env:ProgramFiles 'hull'\r\n"
    "  New-Item -ItemType Directory -Force -Path $bin | Out-Null\r\n"
    "  Copy-Item (Join-Path $gpu 'vgpusrv.exe'), (Join-Path $gpu 'viogpuap.exe') $bin\r\n"
    "  & (Join-Path $bin 'vgpusrv.exe') -i\r\n"
    "} else {\r\n"
    "  Write-Output 'hull: no virtio-win disc with viogpudo found'\r\n"
    "}\r\n";

/* The answer file: the disk wiped and partitioned for UEFI, the edition at
 * `index`, the EULA and the online-account pages passed, an
 * administrator `user` logged on once, which runs hull-setup.ps1 from the
 * stick, whichever drive letter it was given. */
static char *answer(unsigned index, const char *user, const char *secret)
{
	char *text = NULL;

	if (asprintf(&text,
	    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
	    "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\r\n"
	    "          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\r\n"
	    " <settings pass=\"windowsPE\">\r\n"
	    "  <component name=\"Microsoft-Windows-International-Core-WinPE\" processorArchitecture=\"amd64\" "
	    "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\r\n"
	    "   <SetupUILanguage><UILanguage>en-US</UILanguage></SetupUILanguage>\r\n"
	    "   <InputLocale>en-US</InputLocale><SystemLocale>en-US</SystemLocale>\r\n"
	    "   <UILanguage>en-US</UILanguage><UserLocale>en-US</UserLocale>\r\n"
	    "  </component>\r\n"
	    "  <component name=\"Microsoft-Windows-Setup\" processorArchitecture=\"amd64\" "
	    "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\r\n"
	    "   <DiskConfiguration>\r\n"
	    "    <Disk wcm:action=\"add\">\r\n"
	    "     <DiskID>0</DiskID><WillWipeDisk>true</WillWipeDisk>\r\n"
	    "     <CreatePartitions>\r\n"
	    "      <CreatePartition wcm:action=\"add\"><Order>1</Order><Type>EFI</Type><Size>260</Size></CreatePartition>\r\n"
	    "      <CreatePartition wcm:action=\"add\"><Order>2</Order><Type>MSR</Type><Size>16</Size></CreatePartition>\r\n"
	    "      <CreatePartition wcm:action=\"add\"><Order>3</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition>\r\n"
	    "     </CreatePartitions>\r\n"
	    "     <ModifyPartitions>\r\n"
	    "      <ModifyPartition wcm:action=\"add\"><Order>1</Order><PartitionID>1</PartitionID><Format>FAT32</Format><Label>System</Label></ModifyPartition>\r\n"
	    "      <ModifyPartition wcm:action=\"add\"><Order>2</Order><PartitionID>3</PartitionID><Format>NTFS</Format><Label>Windows</Label><Letter>C</Letter></ModifyPartition>\r\n"
	    "     </ModifyPartitions>\r\n"
	    "    </Disk>\r\n"
	    "   </DiskConfiguration>\r\n"
	    "   <ImageInstall><OSImage>\r\n"
	    "    <InstallTo><DiskID>0</DiskID><PartitionID>3</PartitionID></InstallTo>\r\n"
	    "    <InstallFrom><MetaData wcm:action=\"add\"><Key>/IMAGE/INDEX</Key><Value>%u</Value></MetaData></InstallFrom>\r\n"
	    "   </OSImage></ImageInstall>\r\n"
	    "   <UserData><AcceptEula>true</AcceptEula><FullName>%s</FullName><Organization>hull</Organization></UserData>\r\n"
	    "  </component>\r\n"
	    " </settings>\r\n"
	    " <settings pass=\"specialize\">\r\n"
	    "  <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"amd64\" "
	    "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\r\n"
	    "   <ComputerName>HULL-WINDOWS</ComputerName><TimeZone>UTC</TimeZone>\r\n"
	    "  </component>\r\n"
	    /* No device encryption, which Windows 11 turns on by itself:
	     * an encrypted disk's image cannot be compacted from outside
	     * (fmake, 2026-11-18). */
	    "  <component name=\"Microsoft-Windows-Deployment\" processorArchitecture=\"amd64\" "
	    "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\r\n"
	    "   <RunSynchronous><RunSynchronousCommand wcm:action=\"add\"><Order>1</Order>\r\n"
	    "    <Path>reg add HKLM\\SYSTEM\\CurrentControlSet\\Control\\BitLocker /v PreventDeviceEncryption "
	    "/t REG_DWORD /d 1 /f</Path>\r\n"
	    "   </RunSynchronousCommand></RunSynchronous>\r\n"
	    "  </component>\r\n"
	    " </settings>\r\n"
	    " <settings pass=\"oobeSystem\">\r\n"
	    "  <component name=\"Microsoft-Windows-International-Core\" processorArchitecture=\"amd64\" "
	    "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\r\n"
	    "   <InputLocale>en-US</InputLocale><SystemLocale>en-US</SystemLocale>\r\n"
	    "   <UILanguage>en-US</UILanguage><UserLocale>en-US</UserLocale>\r\n"
	    "  </component>\r\n"
	    "  <component name=\"Microsoft-Windows-Shell-Setup\" processorArchitecture=\"amd64\" "
	    "publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\">\r\n"
	    "   <OOBE>\r\n"
	    "    <HideEULAPage>true</HideEULAPage><HideOEMRegistrationScreen>true</HideOEMRegistrationScreen>\r\n"
	    "    <HideOnlineAccountScreens>true</HideOnlineAccountScreens><HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\r\n"
	    "    <ProtectYourPC>3</ProtectYourPC>\r\n"
	    "   </OOBE>\r\n"
	    "   <UserAccounts><LocalAccounts><LocalAccount wcm:action=\"add\">\r\n"
	    "    <Name>%s</Name><Group>Administrators</Group>\r\n"
	    "    <Password><Value>%s</Value><PlainText>true</PlainText></Password>\r\n"
	    "   </LocalAccount></LocalAccounts></UserAccounts>\r\n"
	    "   <AutoLogon><Enabled>true</Enabled><Username>%s</Username><LogonCount>1</LogonCount>\r\n"
	    "    <Password><Value>%s</Value><PlainText>true</PlainText></Password></AutoLogon>\r\n"
	    "   <FirstLogonCommands><SynchronousCommand wcm:action=\"add\"><Order>1</Order>\r\n"
	    "    <CommandLine>cmd /c for %%d in (D E F G H I J K) do if exist %%d:\\hull-setup.ps1 "
	    "powershell -NoProfile -ExecutionPolicy Bypass -File %%d:\\hull-setup.ps1</CommandLine>\r\n"
	    "   </SynchronousCommand></FirstLogonCommands>\r\n"
	    "  </component>\r\n"
	    " </settings>\r\n"
	    "</unattend>\r\n",
	    index, user, user, secret, user, secret) < 0)
		return NULL;
	return text;
}

static int virtio_win_disc(const char *iso, char *path, char *why, size_t cap);
static int virtio_win_stage(const char *iso, const char *stage, char *why, size_t cap);

int hull_setup_windows(const char *iso, const char *dir, unsigned index, const char *user,
                       const char *drivers, char *why, size_t cap)
{
	char        disc[PATH_MAX];
	char        driver_disc[PATH_MAX] = "";
	char       *full_script = NULL;
	char        stage[PATH_MAX];
	char        image[PATH_MAX];
	char        disk[PATH_MAX];
	char        secret[32];
	char       *keys = NULL;
	char       *xml = NULL;
	char       *spec = NULL;
	struct stat st;
	int         result = -1;

	if (user[0] == '\0' || strlen(user) > 20u) {
		snprintf(why, cap, "an account name is 1 to 20 letters, digits, - and _");
		return -1;
	}
	for (const char *c = user; *c != '\0'; c++)
		if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') {
			snprintf(why, cap, "an account name is 1 to 20 letters, digits, - and _");
			return -1;
		}
	if (index == 0u) {
		snprintf(why, cap, "an edition's index counts from 1");
		return -1;
	}
	if (stat(iso, &st) != 0 || !S_ISREG(st.st_mode) || realpath(iso, disc) == NULL) {
		snprintf(why, cap, "%s: not a disc image", iso);
		return -1;
	}
	if (strchr(disc, '"') != NULL || strchr(disc, '\\') != NULL) {
		snprintf(why, cap, "%s: a path with a quote or a backslash cannot be written to a spec", disc);
		return -1;
	}
	if (drivers != NULL && virtio_win_disc(drivers, driver_disc, why, cap) != 0)
		return -1;
	if ((keys = public_keys()) == NULL) {
		snprintf(why, cap, "no public key in ~/.ssh to open the machine's OpenSSH server to: "
		         "make one with ssh-keygen");
		return -1;
	}
	if (mkdir(dir, 0755) != 0) {
		snprintf(why, cap, "%s: %s%s", dir, strerror(errno),
		         errno == EEXIST ? ": setup makes a directory of its own" : "");
		free(keys);
		return -1;
	}
	snprintf(stage, sizeof stage, "%s/answer", dir);
	snprintf(image, sizeof image, "%s/answer.img", dir);
	snprintf(disk, sizeof disk, "%s/disk.qcow2", dir);
	if (password(secret) != 0) {
		snprintf(why, cap, "/dev/urandom could not be read");
		goto done;
	}
	if ((xml = answer(index, user, secret)) == NULL) {
		snprintf(why, cap, "out of memory");
		goto done;
	}
	/* The script: the logon, then the drivers' part where asked, before
	 * its end. */
	{
		const char *end = strstr(script, "Stop-Transcript\r\n");
		size_t      head = (size_t)(end - script);
		char       *logon = NULL;

		if (asprintf(&logon, script_logon, user, secret) < 0)
			logon = NULL;
		if (logon == NULL ||
		    asprintf(&full_script, "%.*s%s%s%s", (int)head, script, logon,
		             driver_disc[0] != '\0' ? script_drivers : "", end) < 0) {
			full_script = NULL;
			free(logon);
			snprintf(why, cap, "out of memory");
			goto done;
		}
		free(logon);
	}
	if (mkdir(stage, 0700) != 0) {
		snprintf(why, cap, "%s: %s", stage, strerror(errno));
		goto done;
	}
	if (put(stage, "autounattend.xml", xml, 0600, why, cap) != 0 ||
	    put(stage, "hull-setup.ps1", full_script, 0600, why, cap) != 0 ||
	    put(stage, "authorized_keys", keys, 0600, why, cap) != 0 ||
	    (driver_disc[0] != '\0' && virtio_win_stage(driver_disc, stage, why, cap) != 0))
		goto done;
	{
		char *line = NULL;

		if (asprintf(&line, "%s\n", secret) < 0 || put(dir, "password", line, 0600, why, cap) != 0) {
			free(line);
			goto done;
		}
		free(line);
	}
	/* The stick: 8 MiB of FAT by mtools, the three files at its root with
	 * their long names, and with --drivers the virtio-win files, 1.5 MB,
	 * under viogpudo as on their disc. */
	{
		int   fd = open(image, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		char  label[] = "ANSWER";
		char *format[] = { "mformat", "-i", image, "-v", label, "::", NULL };

		if (fd < 0 || ftruncate(fd, 8 * 1024 * 1024) != 0) {
			snprintf(why, cap, "%s: %s", image, strerror(errno));
			if (fd >= 0)
				close(fd);
			goto done;
		}
		close(fd);
		if (run(format) != 0) {
			snprintf(why, cap, "mtools' mformat could not make the answer stick: Debian's mtools");
			goto done;
		}
		for (const char *const *f = (const char *const[]){ "autounattend.xml", "hull-setup.ps1",
		                                                   "authorized_keys", "viogpudo", NULL };
		     *f != NULL; f++) {
			char  from[PATH_MAX + 32];
			char  recurse[] = "-s";
			char *copy[] = { "mcopy", recurse, "-i", image, from, "::/", NULL };

			if (strcmp(*f, "viogpudo") == 0 && driver_disc[0] == '\0')
				break;

			snprintf(from, sizeof from, "%s/%s", stage, *f);
			if (run(copy) != 0) {
				snprintf(why, cap, "mtools' mcopy could not put %s on the answer stick", *f);
				goto done;
			}
		}
	}
	{
		char  size[] = "64G";
		char *create[] = { "qemu-img", "create", "-q", "-f", "qcow2", disk, size, NULL };

		if (run(create) != 0) {
			snprintf(why, cap, "qemu-img could not make the machine's disk");
			goto done;
		}
	}
	if (asprintf(&spec,
	    "; Installs Windows from the disc, unattended (hull setup windows): it\n"
	    "; powers off when the first logon's script is done, and then\n"
	    "; machine.conf runs it.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"windows\")\n"
	    "  (source \"media:cdrom\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"8G\")\n"
	    "  (cpus 4)\n"
	    "  (network nat)\n"
	    "  (media\n"
	    "    (medium (type cdrom) (source \"image:%s\"))\n"
	    "    (medium (type disk) (source \"image:./disk.qcow2\"))\n"
	    "    (medium (type usb) (source \"image:./answer.img\")))\n"
	    "  (tuning (pc (machine q35) (cpu host) (tpm true) (firmware uefi-secure)\n"
	    "              (state \"./\") (nic e1000e) (boot_key 30))))\n", disc) < 0 ||
	    put(dir, "install.conf", spec, 0644, why, cap) != 0)
		goto done;
	free(spec);
	spec = NULL;
	if (asprintf(&spec,
	    "; Windows as hull setup windows installed it: ssh -p 2222 to the\n"
	    "; machine's address, the account in the answer, your own keys.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"windows\")\n"
	    "  (source \"image:./disk.qcow2\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"8G\")\n"
	    "  (cpus 4)\n"
	    "  (network nat)\n"
	    "  (ports (port (host 2222) (guest 22)))\n"
	    "  (tuning (pc (machine q35) (cpu host) (tpm true) (firmware uefi-secure)\n"
	    "              (state \"./\") (nic e1000e)%s)))\n",
	    driver_disc[0] != '\0' ? " (vga virtio)" : "") < 0) {
		spec = NULL;
		snprintf(why, cap, "out of memory");
		goto done;
	}
	if (put(dir, "machine.conf", spec, 0644, why, cap) != 0)
		goto done;
	result = 0;
done:
	free(keys);
	free(xml);
	free(spec);
	free(full_script);
	return result;
}

/*
 * `hull setup freedos BOOT DIR`: FreeDOS's own installer has no unattended
 * mode (FDI's stages each ask), so this is FreeDOS's documented install
 * without it, run by the floppy's FDAUTO.BAT with its console on COM1, the
 * run's own output:
 *
 *   FDISK /AUTO 1         a whole-disk partition, active; 0, and the
 *                         machine warm-boots for DOS to see it; 9 on the
 *                         second boot, the disk partitioned already;
 *   FORMAT C: /S          with the system, /Z:seriously answering its
 *                         question for a hard disk;
 *   AUTOEXEC.BAT and FDAPM on C:, and power off.
 *
 * Measured 2026-11-19 on FreeDOS 1.4's floppy edition: 0, then 9 after the
 * warm boot, FORMAT 0, and the disk alone booting to FreeCOM. KEY.COM, eight
 * bytes of INT 16h AH=05, leaves a key pending, which FreeDOS's first read
 * after CTTY waits for (test/legacy_test.sh).
 */
static const char key_com[] = "\264\005\271\040\071\315\026\303";

static const char freedos_auto[] =
    "@echo off\r\n"
    "SET PATH=A:\\FREEDOS\\BIN\r\n"
    "A:\\KEY.COM\r\n"
    "CTTY COM1\r\n"
    "echo hull setup: partitioning C:\r\n"
    "FDISK /AUTO 1\r\n"
    "if errorlevel 9 goto made\r\n"
    "echo hull setup: restarting for DOS to see C:\r\n"
    "FDAPM WARMBOOT\r\n"
    ":made\r\n"
    "echo hull setup: formatting C: with the system\r\n"
    "FORMAT C: /V:FREEDOS /S /Z:seriously\r\n"
    "if errorlevel 1 goto failed\r\n"
    "copy A:\\FREEDOS\\BIN\\FDAPM.COM C:\\ > NUL\r\n"
    "copy A:\\HULLAUTO.BAT C:\\AUTOEXEC.BAT > NUL\r\n"
    "%s"
    "echo hull setup: done\r\n"
    "FDAPM POWEROFF\r\n"
    ":failed\r\n"
    "echo hull setup: FORMAT or the packages' copy failed\r\n";

/* With --packages, after FORMAT: the packages' disk, D:, copied onto C:.
 * Written in only then -- a test of D: in the script, `if not exist
 * D:\FREEDOS\NUL`, took the branch on a machine with no D: and once
 * hung the install at a critical-error prompt (2026-11-20). */
static const char freedos_packages[] =
    "echo hull setup: installing the base packages\r\n"
    "A:\\FREEDOS\\BIN\\XCOPY D:\\FREEDOS C:\\FREEDOS /S /E /I /Y > NUL\r\n"
    "if errorlevel 1 goto failed\r\n";

/* What the installed disk boots to: its prompt on the screen, the base
 * packages on its PATH where they were installed. */
static const char freedos_disk_auto[] =
    "@echo off\r\n"
    "SET DOSDIR=C:\\FREEDOS\r\n"
    "SET PATH=C:\\;C:\\FREEDOS\\BIN\r\n"
    "SET TEMP=C:\\\r\n"
    "PROMPT $P$G\r\n";

/* Every name under `dir` upper-cased, as DOS names are and FAT folds
 * them: packages unpack some into nls/ and others into NLS/, which on the
 * FAT disk are one directory, so a directory whose upper-cased name is
 * taken is merged into it. 0, or -1. */
static int upcase_tree(const char *dir)
{
	DIR           *d = opendir(dir);
	struct dirent *e;
	int            status = 0;

	if (d == NULL)
		return -1;
	while (status == 0 && (e = readdir(d)) != NULL) {
		char        from[PATH_MAX], to[PATH_MAX], upper[256];
		struct stat is, there;
		size_t      i;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		for (i = 0u; e->d_name[i] != '\0' && i + 1u < sizeof upper; i++)
			upper[i] = (char)toupper((unsigned char)e->d_name[i]);
		upper[i] = '\0';
		snprintf(from, sizeof from, "%s/%s", dir, e->d_name);
		snprintf(to, sizeof to, "%s/%s", dir, upper);
		if (lstat(from, &is) != 0) {
			status = -1;
			break;
		}
		if (strcmp(from, to) != 0) {
			if (lstat(to, &there) == 0) {
				/* Taken: a directory merges into it, entry by entry,
				 * then goes; a file of the same name is the one already
				 * there, as XCOPY /Y would leave the later. */
				if (S_ISDIR(is.st_mode) && S_ISDIR(there.st_mode)) {
					DIR           *inner = opendir(from);
					struct dirent *f;

					if (inner == NULL) {
						status = -1;
						break;
					}
					while (status == 0 && (f = readdir(inner)) != NULL) {
						char a[PATH_MAX + 256], b[PATH_MAX + 256];

						if (strcmp(f->d_name, ".") == 0 || strcmp(f->d_name, "..") == 0)
							continue;
						snprintf(a, sizeof a, "%s/%s", from, f->d_name);
						snprintf(b, sizeof b, "%s/%s", to, f->d_name);
						status = rename(a, b) == 0 ? 0 : -1;
					}
					closedir(inner);
					if (status == 0 && rmdir(from) != 0)
						status = -1;
					/* The merged entries are renamed again below. */
					rewinddir(d);
					continue;
				}
				status = rename(from, to) == 0 ? 0 : -1;
			} else {
				status = rename(from, to) == 0 ? 0 : -1;
			}
		}
		if (status == 0 && S_ISDIR(is.st_mode))
			status = upcase_tree(to);
	}
	closedir(d);
	return status;
}

/* FreeDOS's base packages from `image` -- a FreeDOS 1.4 USB image, its
 * FAT partition holding packages/base's zips, as the Lite USB edition's
 * FD14LITE.img does -- laid out as %DOSDIR% would hold them installed:
 * each zip unpacked into DIR/packages/FREEDOS but its SOURCE, and its
 * files listed in PACKAGES\NAME.LST, C:\FreeDOS\... a line, as FreeDOS's
 * installer writes them. Then that tree as a disk of its own,
 * DIR/packages.img, one FAT16 partition by `hull disk write`, which the
 * install's script copies onto C: by XCOPY: the zips need no unzip in
 * DOS, and FreeDOS's own tools find the packages as installed. */
static int stage_packages(const char *image, const char *dir, char *why, size_t cap)
{
	char          zips[PATH_MAX], top[PATH_MAX], tree[PATH_MAX + 16], lists[PATH_MAX + 32];
	char          at[PATH_MAX + 32];
	char          self[PATH_MAX];
	uint8_t       mbr[512];
	unsigned long first = 0ul;
	ssize_t       n;
	int           fd;
	DIR          *d;
	struct dirent *e;
	unsigned      count = 0u;

	if ((fd = open(image, O_RDONLY | O_CLOEXEC)) < 0 || read(fd, mbr, sizeof mbr) != 512) {
		snprintf(why, cap, "%s: %s", image, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	/* The first partition the table names, or a disk of no table. */
	if (mbr[510] == 0x55u && mbr[511] == 0xaau)
		for (unsigned i = 0u; i < 4u && first == 0ul; i++)
			if (mbr[446 + 16u * i + 4u] != 0u)
				first = (unsigned long)mbr[454 + 16u * i] | (unsigned long)mbr[455 + 16u * i] << 8 |
				        (unsigned long)mbr[456 + 16u * i] << 16 |
				        (unsigned long)mbr[457 + 16u * i] << 24;
	snprintf(at, sizeof at, "%s@@%lu", image, first * 512ul);
	snprintf(zips, sizeof zips, "%s/packages-zip", dir);
	snprintf(top, sizeof top, "%s/packages", dir);
	snprintf(tree, sizeof tree, "%s/FREEDOS", top);
	snprintf(lists, sizeof lists, "%s/PACKAGES", tree);
	if (mkdir(zips, 0755) != 0 || mkdir(top, 0755) != 0 || mkdir(tree, 0755) != 0 ||
	    mkdir(lists, 0755) != 0) {
		snprintf(why, cap, "%s: %s", dir, strerror(errno));
		return -1;
	}
	{
		char *copy[] = { "env", "MTOOLSRC=/dev/null", "mcopy", "-n", "-i", at,
		                 "::/packages/base/*.zip", zips, NULL };

		if (run(copy) != 0) {
			snprintf(why, cap, "%s has no packages/base/*.zip that mtools can read: FreeDOS "
			         "1.4's Lite USB image, FD14LITE.img, has", image);
			return -1;
		}
	}
	if ((d = opendir(zips)) == NULL) {
		snprintf(why, cap, "%s: %s", zips, strerror(errno));
		return -1;
	}
	while ((e = readdir(d)) != NULL) {
		char   zip[PATH_MAX + 256];
		char   list[PATH_MAX + 256];
		char   name[64];
		char   line[1024];
		char   command[PATH_MAX + 300];
		int    code;
		size_t length = strlen(e->d_name);
		FILE  *files;
		FILE  *out;
		char  *unpack[] = { "unzip", "-q", "-o", zip, "-x", "SOURCE/*", "source/*", "-d", tree,
		                    NULL };

		if (length < 5u || length > 60u || strcasecmp(e->d_name + length - 4u, ".zip") != 0)
			continue;
		snprintf(zip, sizeof zip, "%s/%s", zips, e->d_name);
		for (size_t i = 0u; i + 4u < length; i++)
			name[i] = (char)toupper((unsigned char)e->d_name[i]);
		name[length - 4u] = '\0';
		/* unzip exits 11 where an excluded name matched nothing -- a
		 * package with no SOURCE -- and has unpacked all the same. */
		code = run_code(unpack);
		if (code != 0 && code != 11) {
			closedir(d);
			snprintf(why, cap, "%s could not be unpacked (unzip)", zip);
			return -1;
		}
		/* Its list: each file the zip holds but its sources. */
		snprintf(command, sizeof command, "unzip -Z1 '%s'", zip);
		snprintf(list, sizeof list, "%s/%s.LST", lists, name);
		if (strchr(zip, '\'') != NULL || (files = popen(command, "re")) == NULL) {
			closedir(d);
			snprintf(why, cap, "%s could not be listed", zip);
			return -1;
		}
		if ((out = fopen(list, "we")) == NULL) {
			pclose(files);
			closedir(d);
			snprintf(why, cap, "%s: %s", list, strerror(errno));
			return -1;
		}
		while (fgets(line, sizeof line, files) != NULL) {
			size_t k = strcspn(line, "\r\n");

			line[k] = '\0';
			if (k == 0u || line[k - 1u] == '/' || strncasecmp(line, "SOURCE/", 7u) == 0)
				continue;
			for (char *c = line; *c != '\0'; c++)
				*c = *c == '/' ? '\\' : (char)toupper((unsigned char)*c);
			fprintf(out, "C:\\FreeDOS\\%s\r\n", line);
		}
		pclose(files);
		if (fclose(out) != 0) {
			closedir(d);
			snprintf(why, cap, "%s could not be written", list);
			return -1;
		}
		count++;
	}
	closedir(d);
	if (count == 0u) {
		snprintf(why, cap, "%s's packages/base holds no package", image);
		return -1;
	}
	if (upcase_tree(tree) != 0) {
		snprintf(why, cap, "%s's names could not be made DOS's: %s", tree, strerror(errno));
		return -1;
	}
	if (put(dir, "packages.conf",
	    "; FreeDOS's base packages as installed, a disk the install copies onto C:\n"
	    "; (hull setup freedos --packages).\n"
	    "(disk (label dos)\n"
	    "  (partitions\n"
	    "    (partition (size \"rest\") (type fat) (volume \"PACKAGES\") (source \"dir:./packages\"))))\n",
	    0644, why, cap) != 0)
		return -1;
	n = readlink("/proc/self/exe", self, sizeof self - 1u);
	if (n <= 0) {
		snprintf(why, cap, "hull cannot find itself to write the packages' disk");
		return -1;
	}
	self[n] = '\0';
	{
		char conf[PATH_MAX + 32];
		char disk[PATH_MAX + 32];
		char *write[] = { self, "disk", "write", conf, disk, NULL };
		char *clear[] = { "rm", "-rf", "--", zips, NULL };

		snprintf(conf, sizeof conf, "%s/packages.conf", dir);
		snprintf(disk, sizeof disk, "%s/packages.img", dir);
		if (run(write) != 0) {
			snprintf(why, cap, "the packages' disk could not be written: `hull disk write %s %s` "
			         "says why", conf, disk);
			return -1;
		}
		/* The zips are the image's still; the tree stays, the disk's
		 * source, for a look at what was installed. */
		run(clear);
	}
	return 0;
}

int hull_setup_freedos(const char *boot, const char *dir, unsigned megabytes, const char *packages,
                       char *why, size_t cap)
{
	char        install[1024];
	char        fdauto[sizeof freedos_auto + sizeof freedos_packages];
	char        floppy[PATH_MAX];
	char        disk[PATH_MAX];
	char        stage[PATH_MAX];
	struct stat st;
	int         fd;

	if (megabytes < 8u || megabytes > 2047u) {
		snprintf(why, cap, "FreeDOS's disk is 8 to 2047 MB, one FAT16 partition");
		return -1;
	}
	if (stat(boot, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 1474560) {
		snprintf(why, cap, "%s: not a 1.44 MB floppy image, as FreeDOS's x86BOOT.img is", boot);
		return -1;
	}
	if (mkdir(dir, 0755) != 0) {
		snprintf(why, cap, "%s: %s%s", dir, strerror(errno),
		         errno == EEXIST ? ": setup makes a directory of its own" : "");
		return -1;
	}
	snprintf(floppy, sizeof floppy, "%s/setup.img", dir);
	snprintf(disk, sizeof disk, "%s/disk.img", dir);
	snprintf(stage, sizeof stage, "%s/setup", dir);
	if (mkdir(stage, 0755) != 0) {
		snprintf(why, cap, "%s: %s", stage, strerror(errno));
		return -1;
	}
	/* The user's floppy copied, theirs left as it was. */
	{
		char *copy[] = { "cp", (char *)boot, floppy, NULL };

		if (run(copy) != 0 || chmod(floppy, 0644) != 0) {
			snprintf(why, cap, "%s could not be copied", boot);
			return -1;
		}
	}
	snprintf(fdauto, sizeof fdauto, freedos_auto, packages != NULL ? freedos_packages : "");
	if (put(stage, "KEY.COM", key_com, 0644, why, cap) != 0 ||
	    put(stage, "FDAUTO.BAT", fdauto, 0644, why, cap) != 0 ||
	    put(stage, "HULLAUTO.BAT", freedos_disk_auto, 0644, why, cap) != 0)
		return -1;
	for (const char *const *f = (const char *const[]){ "KEY.COM", "FDAUTO.BAT", "HULLAUTO.BAT",
	                                                   NULL }; *f != NULL; f++) {
		char  from[PATH_MAX + 32];
		char  to[64];
		char *copy[] = { "mcopy", "-o", "-i", floppy, from, to, NULL };

		snprintf(from, sizeof from, "%s/%s", stage, *f);
		snprintf(to, sizeof to, "::%s", *f);
		if (run(copy) != 0) {
			snprintf(why, cap, "mtools' mcopy could not put %s on the floppy: is it FAT?", *f);
			return -1;
		}
	}
	if (packages != NULL && stage_packages(packages, dir, why, cap) != 0)
		return -1;
	if ((fd = open(disk, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0 ||
	    ftruncate(fd, (off_t)megabytes * 1024 * 1024) != 0) {
		snprintf(why, cap, "%s: %s", disk, strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	close(fd);
	snprintf(install, sizeof install,
	    "; Installs FreeDOS onto disk.img from the floppy, unattended (hull setup\n"
	    "; freedos): it partitions, restarts, formats and powers off. Then\n"
	    "; machine.conf boots the disk.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"freedos\")\n"
	    "  (source \"media:floppy\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"32M\")\n"
	    "  (media\n"
	    "    (medium (type floppy) (source \"image:./setup.img\"))\n"
	    "    (medium (type disk) (source \"image:./disk.img\"))%s))\n",
	         packages != NULL ? "\n    (medium (type disk) (source \"image:./packages.img\"))" : "");
	if (put(dir, "install.conf", install, 0644, why, cap) != 0 ||
	    put(dir, "machine.conf",
	    "; FreeDOS as hull setup freedos installed it.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"freedos\")\n"
	    "  (source \"media:disk\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"32M\")\n"
	    "  (media\n"
	    "    (medium (type disk) (source \"image:./disk.img\"))))\n", 0644, why, cap) != 0)
		return -1;
	return 0;
}

/*
 * `hull setup reactos ISO DIR`: ReactOS's own unattended setup, which its
 * boot CD carries switched off -- i386/unattend.inf saying
 * `UnattendSetupEnabled = no` -- and its boot menu starting the LiveCD
 * rather than setup. Both are files on the disc, so a copy of the disc has
 * the two rewritten where they lie: each file's sectors are its own, a
 * file starting on a sector of its own, so a file may grow to the end of
 * its last, and its length is written into every directory record naming
 * it -- the primary volume's and the ISO 9660:1999 or Joliet one's. No
 * ISO is mastered again, so the boot record and every other byte stay as
 * the user's disc has them.
 */
#define DISC_SECTOR 2048u

/* A directory record of the disc found by its path. */
struct disc_file {
	off_t    records[4];  /* where each record naming it lies */
	unsigned count;
	uint32_t extent;      /* its first sector */
	uint32_t length;      /* its bytes */
};

static uint32_t le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Whether a record's identifier is `want`, a name of ASCII: compared
 * without case, the ";1" of its version and a bare name's trailing "."
 * left off; a Joliet one is UCS-2, big-endian. */
static int disc_named(const unsigned char *id, unsigned n, int joliet, const char *want)
{
	char     name[128];
	unsigned length = 0u;

	if (joliet) {
		if (n % 2u != 0u)
			return 0;
		for (unsigned i = 0u; i < n; i += 2u) {
			if (id[i] != 0u || length + 1u >= sizeof name)
				return 0;
			name[length++] = (char)id[i + 1u];
		}
	} else {
		if (n >= sizeof name)
			return 0;
		memcpy(name, id, n);
		length = n;
	}
	name[length] = '\0';
	if (strchr(name, ';') != NULL)
		*strchr(name, ';') = '\0';
	length = (unsigned)strlen(name);
	if (length > 0u && name[length - 1u] == '.')
		name[--length] = '\0';
	return strcasecmp(name, want) == 0;
}

/* `path`'s records under the root record `root` of one volume
 * descriptor: 0 with its record's offset added to `file`, 1 where the
 * volume has no such file, -1 where the disc cannot be read. */
static int disc_walk(int fd, off_t size, const unsigned char *root, int joliet, const char *path,
                     struct disc_file *file)
{
	uint32_t extent = le32(root + 2), length = le32(root + 10);
	char     part[128];

	while (*path != '\0') {
		const char *slash = strchr(path, '/');
		size_t      n = slash != NULL ? (size_t)(slash - path) : strlen(path);
		int         found = 0;

		if (n == 0u || n >= sizeof part)
			return 1;
		memcpy(part, path, n);
		part[n] = '\0';
		path += n + (slash != NULL ? 1u : 0u);
		if (length > (16u << 20) || (off_t)extent * DISC_SECTOR + length > size)
			return -1;
		for (uint32_t s = 0u; !found && s < (length + DISC_SECTOR - 1u) / DISC_SECTOR; s++) {
			unsigned char sector[DISC_SECTOR];
			off_t         at = ((off_t)extent + s) * DISC_SECTOR;

			if (pread(fd, sector, sizeof sector, at) != (ssize_t)sizeof sector)
				return -1;
			for (unsigned i = 0u; i < DISC_SECTOR && sector[i] != 0u; i += sector[i]) {
				unsigned char *r = sector + i;
				int            directory;

				if (sector[i] < 34u || i + sector[i] > DISC_SECTOR || 33u + r[32] > sector[i])
					return -1;
				directory = (r[25] & 2u) != 0u;
				if (directory != (*path != '\0') || !disc_named(r + 33, r[32], joliet, part))
					continue;
				if (*path == '\0') {
					if (file->count == sizeof file->records / sizeof file->records[0])
						return -1;
					file->records[file->count++] = at + i;
				}
				extent = le32(r + 2);
				length = le32(r + 10);
				found = 1;
				break;
			}
		}
		if (!found)
			return 1;
	}
	if (file->count > 1u && (extent != file->extent || length != file->length))
		return -1;  /* two volumes disagree on where it is */
	file->extent = extent;
	file->length = length;
	return 0;
}

/* `path` on the disc, by every volume descriptor naming it: 0, or -1. */
static int disc_find(int fd, const char *path, struct disc_file *file)
{
	struct stat st;

	memset(file, 0, sizeof *file);
	if (fstat(fd, &st) != 0)
		return -1;
	for (unsigned s = 16u; s < 32u; s++) {
		unsigned char d[DISC_SECTOR];
		int           joliet;

		if (pread(fd, d, sizeof d, (off_t)s * DISC_SECTOR) != (ssize_t)sizeof d ||
		    memcmp(d + 1, "CD001", 5u) != 0 || d[0] == 255u)
			break;
		if (d[0] != 1u && d[0] != 2u)
			continue;
		joliet = d[0] == 2u && d[88] == '%' && d[89] == '/';
		if (disc_walk(fd, st.st_size, d + 156, joliet, path, file) < 0)
			return -1;
	}
	return file->count > 0u && file->length > 0u &&
	       (off_t)file->extent * DISC_SECTOR + file->length <= st.st_size ? 0 : -1;
}

/* `path`'s bytes, allocated and terminated: NULL where it is not there. */
static char *disc_read(int fd, const char *path)
{
	struct disc_file file;
	char            *text;

	if (disc_find(fd, path, &file) != 0 || file.length > (1u << 20) ||
	    (text = malloc(file.length + 1u)) == NULL)
		return NULL;
	if (pread(fd, text, file.length, (off_t)file.extent * DISC_SECTOR) != (ssize_t)file.length) {
		free(text);
		return NULL;
	}
	text[file.length] = '\0';
	return text;
}

/* What --drivers takes off a virtio-win disc, for both releases' amd64:
 * the display driver, its catalogue, the resolution service and the
 * program the service starts in a user's session -- not the debug
 * symbols beside them, 30 MB of the disc. */
static const char *const virtio_win_files[] = {
	"viogpudo/w11/amd64/viogpudo.inf", "viogpudo/w11/amd64/viogpudo.sys",
	"viogpudo/w11/amd64/viogpudo.cat", "viogpudo/w11/amd64/vgpusrv.exe",
	"viogpudo/w11/amd64/viogpuap.exe",
	"viogpudo/w10/amd64/viogpudo.inf", "viogpudo/w10/amd64/viogpudo.sys",
	"viogpudo/w10/amd64/viogpudo.cat", "viogpudo/w10/amd64/vgpusrv.exe",
	"viogpudo/w10/amd64/viogpuap.exe", NULL
};

/* --drivers' disc: a virtio-win disc, Red Hat's, holding the display
 * driver and its service for both releases' amd64, its real path in `path`
 * (PATH_MAX): 0, or -1 with `why`. Read here rather than an hour into the
 * install, where a wrong disc would leave the plain VGA silently. */
static int virtio_win_disc(const char *iso, char *path, char *why, size_t cap)
{
	struct stat st;
	int         fd;

	if (stat(iso, &st) != 0 || !S_ISREG(st.st_mode) || realpath(iso, path) == NULL) {
		snprintf(why, cap, "%s: not a disc image", iso);
		return -1;
	}
	if (strchr(path, '"') != NULL || strchr(path, '\\') != NULL) {
		snprintf(why, cap, "%s: a path with a quote or a backslash cannot be written to a spec", path);
		return -1;
	}
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	for (const char *const *w = virtio_win_files; *w != NULL; w++) {
		struct disc_file file;

		if (disc_find(fd, *w, &file) != 0 || file.length > (1u << 20)) {
			snprintf(why, cap, "%s: not a virtio-win disc: no %s on it", iso, *w);
			close(fd);
			return -1;
		}
	}
	close(fd);
	return 0;
}

/* virtio_win_files copied off the disc into `stage`, at the disc's own
 * paths, for the answer stick: 0, or -1 with `why`. */
static int virtio_win_stage(const char *iso, const char *stage, char *why, size_t cap)
{
	int fd = open(iso, O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		snprintf(why, cap, "%s: %s", iso, strerror(errno));
		return -1;
	}
	for (const char *const *w = virtio_win_files; *w != NULL; w++) {
		struct disc_file file;
		char             to[PATH_MAX];
		char            *bytes = NULL;
		int              out = -1;

		/* Its directories, each made on the way. */
		for (const char *slash = strchr(*w, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
			int at = snprintf(to, sizeof to, "%s/%.*s", stage, (int)(slash - *w), *w);

			if (at < 0 || (size_t)at >= sizeof to || (mkdir(to, 0700) != 0 && errno != EEXIST)) {
				snprintf(why, cap, "%s: %s", to, at < 0 || (size_t)at >= sizeof to ?
				         "too long a path" : strerror(errno));
				close(fd);
				return -1;
			}
		}
		if (snprintf(to, sizeof to, "%s/%s", stage, *w) >= (int)sizeof to ||
		    disc_find(fd, *w, &file) != 0 || file.length > (1u << 20) ||
		    (bytes = malloc(file.length + 1u)) == NULL ||
		    pread(fd, bytes, file.length, (off_t)file.extent * DISC_SECTOR) != (ssize_t)file.length ||
		    (out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0 ||
		    write(out, bytes, file.length) != (ssize_t)file.length || close(out) != 0) {
			snprintf(why, cap, "%s: %s could not be copied to the answer stick", iso, *w);
			if (out >= 0)
				close(out);
			free(bytes);
			close(fd);
			return -1;
		}
		free(bytes);
	}
	close(fd);
	return 0;
}

/* `path` rewritten as `text`, which must fit its sectors: 0, or -1 with
 * `why`. */
static int disc_rewrite(int fd, const char *path, const char *text, char *why, size_t cap)
{
	struct disc_file file;
	size_t           length = strlen(text);
	size_t           room;
	char            *sectors;
	unsigned char    both[8];

	if (disc_find(fd, path, &file) != 0) {
		snprintf(why, cap, "%s is not on the disc: not a ReactOS boot CD", path);
		return -1;
	}
	room = ((size_t)file.length + DISC_SECTOR - 1u) / DISC_SECTOR * DISC_SECTOR;
	if (length > room) {
		snprintf(why, cap, "%s: %zu bytes do not fit the %zu its sectors hold", path, length, room);
		return -1;
	}
	if ((sectors = calloc(1u, room)) == NULL) {
		snprintf(why, cap, "out of memory");
		return -1;
	}
	memcpy(sectors, text, length);
	for (unsigned i = 0u; i < 4u; i++) {
		both[i] = (unsigned char)(length >> (8u * i));
		both[7u - i] = (unsigned char)(length >> (8u * i));
	}
	if (pwrite(fd, sectors, room, (off_t)file.extent * DISC_SECTOR) != (ssize_t)room) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		free(sectors);
		return -1;
	}
	free(sectors);
	for (unsigned i = 0u; i < file.count; i++)
		if (pwrite(fd, both, sizeof both, file.records[i] + 10) != (ssize_t)sizeof both) {
			snprintf(why, cap, "%s: %s", path, strerror(errno));
			return -1;
		}
	return 0;
}

/* ReactOS's own answer file, switched on: the whole of the first disk one
 * partition, formatted FAT, the system in C:\ReactOS, the boot loader in
 * the MBR and the partition's boot sector; and, at the first logon after
 * setup, a power-off, which ends the install's run. */
static const char reactos_unattend[] =
    "; Written by hull setup reactos: ReactOS's own unattended setup,\r\n"
    "; i386/unattend.inf on its boot CD, switched on.\r\n"
    "[Unattend]\r\n"
    "Signature = \"$ReactOS$\"\r\n"
    "UnattendSetupEnabled = yes\r\n"
    "DestinationDiskNumber = 0\r\n"
    "DestinationPartitionNumber = 1\r\n"
    "InstallationDirectory = ReactOS\r\n"
    "BootLoaderLocation = 2\r\n"
    "FullName = \"hull\"\r\n"
    "ComputerName = \"REACTOS\"\r\n"
    "TimeZoneIndex = 85\r\n"
    "FormatPartition = 1\r\n"
    "AutoPartition = 1\r\n"
    "FsType = 0\r\n"
    "RappsDownload = no\r\n"
    "LocaleID = 409\r\n"
    "InstallationType = 1\r\n"
    "\r\n"
    "[GuiRunOnce]\r\n"
    "%SystemRoot%\\system32\\shutdown.exe /s /t 0\r\n";

/* freeldr.ini with its menu's default `Setup`, the text-mode setup that
 * reads the answer file, in place of the LiveCD: allocated, or NULL where
 * the menu has no DefaultOS or no Setup entry to choose. */
static char *reactos_menu(const char *ini)
{
	const char *line = ini;
	const char *at = NULL;
	int         setup = 0;
	char       *menu;

	while (*line != '\0') {
		const char *end = line + strcspn(line, "\r\n");

		if (strncasecmp(line, "DefaultOS=", 10u) == 0 && at == NULL)
			at = line;
		if (strncasecmp(line, "[Setup]", 7u) == 0)
			setup = 1;
		line = end + strspn(end, "\r\n");
	}
	if (at == NULL || !setup)
		return NULL;
	if (asprintf(&menu, "%.*sDefaultOS=Setup%s", (int)(at - ini), ini, at + strcspn(at, "\r\n")) < 0)
		return NULL;
	return menu;
}

int hull_setup_reactos(const char *iso, const char *dir, unsigned megabytes, char *why, size_t cap)
{
	char        disc[PATH_MAX];
	char        disk[PATH_MAX];
	char       *ini = NULL;
	char       *menu = NULL;
	struct stat st;
	int         fd = -1;
	int         result = -1;

	if (megabytes < 1024u || megabytes > 32768u) {
		snprintf(why, cap, "ReactOS's disk is 1024 to 32768 MB, one FAT32 partition");
		return -1;
	}
	if (stat(iso, &st) != 0 || !S_ISREG(st.st_mode)) {
		snprintf(why, cap, "%s: not a disc image", iso);
		return -1;
	}
	if (mkdir(dir, 0755) != 0) {
		snprintf(why, cap, "%s: %s%s", dir, strerror(errno),
		         errno == EEXIST ? ": setup makes a directory of its own" : "");
		return -1;
	}
	snprintf(disc, sizeof disc, "%s/setup.iso", dir);
	snprintf(disk, sizeof disk, "%s/disk.img", dir);
	/* The user's disc copied, theirs left as it was; a reflink where the
	 * filesystem has them. */
	{
		char *copy[] = { "cp", "--reflink=auto", (char *)iso, disc, NULL };

		if (run(copy) != 0 || chmod(disc, 0644) != 0) {
			snprintf(why, cap, "%s could not be copied", iso);
			goto done;
		}
	}
	if ((fd = open(disc, O_RDWR | O_CLOEXEC)) < 0) {
		snprintf(why, cap, "%s: %s", disc, strerror(errno));
		goto done;
	}
	if ((ini = disc_read(fd, "freeldr.ini")) == NULL || (menu = reactos_menu(ini)) == NULL) {
		snprintf(why, cap, "%s: no freeldr.ini with a Setup entry: not a ReactOS boot CD", iso);
		goto done;
	}
	if (disc_rewrite(fd, "i386/unattend.inf", reactos_unattend, why, cap) != 0 ||
	    disc_rewrite(fd, "freeldr.ini", menu, why, cap) != 0)
		goto done;
	if (close(fd) != 0) {
		fd = -1;
		snprintf(why, cap, "%s: %s", disc, strerror(errno));
		goto done;
	}
	fd = -1;
	if ((fd = open(disk, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0 ||
	    ftruncate(fd, (off_t)megabytes * 1024 * 1024) != 0) {
		snprintf(why, cap, "%s: %s", disk, strerror(errno));
		goto done;
	}
	close(fd);
	fd = -1;
	if (put(dir, "install.conf",
	    "; Installs ReactOS onto disk.img from setup.iso, unattended (hull\n"
	    "; setup reactos): its text-mode setup, a restart, its graphical\n"
	    "; setup, a restart, and a power-off at the first logon. Its log is\n"
	    "; the run's output, the setup's debug port being COM1. Then\n"
	    "; machine.conf boots the disk.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"reactos\")\n"
	    "  (source \"media:cdrom\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"1G\")\n"
	    "  (network nat)\n"
	    "  (media\n"
	    "    (medium (type cdrom) (source \"image:./setup.iso\"))\n"
	    "    (medium (type disk) (source \"image:./disk.img\"))))\n", 0644, why, cap) != 0 ||
	    put(dir, "machine.conf",
	    "; ReactOS as hull setup reactos installed it.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"reactos\")\n"
	    "  (source \"media:disk\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"1G\")\n"
	    "  (network nat)\n"
	    "  (media\n"
	    "    (medium (type disk) (source \"image:./disk.img\"))))\n", 0644, why, cap) != 0)
		goto done;
	result = 0;
done:
	if (fd >= 0)
		close(fd);
	free(ini);
	free(menu);
	/* Refused, nothing of it left: what it made, by name, and DIR. */
	if (result != 0) {
		char path[PATH_MAX + 32];

		for (const char *const *f = (const char *const[]){ "setup.iso", "disk.img", "install.conf",
		                                                   "machine.conf", NULL }; *f != NULL; f++) {
			snprintf(path, sizeof path, "%s/%s", dir, *f);
			unlink(path);
		}
		rmdir(dir);
	}
	return result;
}

/*
 * `hull setup msdos [--size MB] BOOT DIR`: MS-DOS or PC DOS installed from
 * the user's own boot floppy, by the floppy's own FORMAT. DOS's FDISK has
 * no unattended mode before Windows 9x's, so the table is hull's: one
 * primary partition, active, from the first track to the disk's last
 * cylinder, of type 04h under 65536 sectors and 06h above -- the two every
 * DOS from 3.3 on reads, where hull's own `(disk ...)` writes FAT16 as
 * 0Eh, LBA, which no DOS before 7 knows -- with its CHS in 16 heads and 63
 * sectors, the geometry qemu then takes from the table, and SYSLINUX's
 * MBR code to start the active partition. A copy of the floppy carries an
 * AUTOEXEC.BAT that runs `FORMAT C: /S` with its two answers from a file,
 * copies the floppy into C:\DOS and powers off by APM.
 *
 * Measured 2026-11-21 on MS-DOS 4.00 built from Microsoft's MIT-licensed
 * source (test/legacy_test.sh): FORMAT reading Y and the label from
 * HULLY.TXT, the system transferred, and the disk alone booting to its
 * own AUTOEXEC.BAT.
 */

/* APM by INT 15h: connect the real-mode interface (5301h), say version
 * 1.2 (530Eh), set all devices off (5307h, BX=1, CX=3); INT 20h if the
 * BIOS has no APM. SeaBIOS's off is ACPI's sleep state 5. No zero byte,
 * put() writing a string: BX by XOR and INC, CX by XOR and MOV CL. */
static const char msdos_off[] =
    "\270\001\123\061\333\315\025"          /* mov ax,5301h; xor bx,bx; int 15h */
    "\270\016\123\061\333\271\002\001\315\025"  /* mov ax,530Eh; xor bx,bx; mov cx,0102h */
    "\270\007\123\061\333\103\061\311\261\003\315\025"  /* 5307h, bx=1, cx=3 */
    "\315\040";                                /* int 20h */

static const char msdos_answers[] = "Y\r\nMSDOS\r\n";

static const char msdos_auto[] =
    "@ECHO OFF\r\n"
    "VER >COM1\r\n"
    "ECHO hull setup: formatting C: with the system >COM1\r\n"
    "FORMAT C: /S <A:\\HULLY.TXT >COM1\r\n"
    "IF ERRORLEVEL 1 GOTO FAILED\r\n"
    "ECHO hull setup: copying the floppy into C:\\DOS >COM1\r\n"
    "MD C:\\DOS\r\n"
    "COPY A:\\*.* C:\\DOS >NUL\r\n"
    "COPY A:\\HULLAUTO.BAT C:\\AUTOEXEC.BAT >NUL\r\n"
    "ECHO hull setup: done >COM1\r\n"
    "A:\\HULLOFF.COM\r\n"
    ":FAILED\r\n"
    "ECHO hull setup: FORMAT failed >COM1\r\n"
    "A:\\HULLOFF.COM\r\n";

static const char msdos_disk_auto[] =
    "@ECHO OFF\r\n"
    "PATH C:\\DOS;C:\\\r\n"
    "PROMPT $P$G\r\n";

/* The MBR: SYSLINUX's code, one partition from track 1 to the last whole
 * cylinder, active. `sectors` is the disk's. */
static int msdos_table(int fd, uint32_t sectors, char *why, size_t cap)
{
	static const char code_path[] = "/usr/lib/syslinux/mbr/mbr.bin";
	unsigned char     mbr[512] = { 0 };
	uint32_t          start = 63u, end = sectors - 1u, count = sectors - 63u;
	int               code = open(code_path, O_RDONLY | O_CLOEXEC);
	ssize_t           n;

	if (code < 0) {
		snprintf(why, cap, "%s: %s: Debian's syslinux-common has the MBR code", code_path,
		         strerror(errno));
		return -1;
	}
	n = read(code, mbr, 440);
	close(code);
	if (n != 440) {
		snprintf(why, cap, "%s: not 440 bytes of MBR code", code_path);
		return -1;
	}
	{
		unsigned char *e = mbr + 446;
		uint32_t       at[2] = { start, end };

		e[0] = 0x80;
		for (unsigned i = 0u; i < 2u; i++) {
			uint32_t       c = at[i] / (16u * 63u), h = at[i] / 63u % 16u, s = at[i] % 63u + 1u;
			unsigned char *chs = e + (i == 0u ? 1 : 5);

			chs[0] = (unsigned char)h;
			chs[1] = (unsigned char)(s | ((c >> 2) & 0xc0u));
			chs[2] = (unsigned char)(c & 0xffu);
		}
		e[4] = count < 65536u ? 0x04 : 0x06;
		for (unsigned i = 0u; i < 4u; i++) {
			e[8 + i] = (unsigned char)(start >> (8u * i));
			e[12 + i] = (unsigned char)(count >> (8u * i));
		}
	}
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	if (pwrite(fd, mbr, sizeof mbr, 0) != (ssize_t)sizeof mbr) {
		snprintf(why, cap, "the disk's table could not be written: %s", strerror(errno));
		return -1;
	}
	return 0;
}

int hull_setup_msdos(const char *boot, const char *dir, unsigned megabytes, char *why, size_t cap)
{
	static const off_t sizes[] = { 368640, 737280, 1228800, 1474560, 2949120 };
	char               floppy[PATH_MAX];
	char               disk[PATH_MAX];
	char               stage[PATH_MAX];
	struct stat        st;
	uint32_t           sectors;
	int                fd = -1;
	int                result = -1;
	int                sized = 0;

	if (megabytes < 2u || megabytes > 504u) {
		snprintf(why, cap, "DOS's disk is 2 to 504 MB, one primary partition under 1024 cylinders "
		         "of 16 heads (MS-DOS before 4.0 reads 32 MB at most)");
		return -1;
	}
	if (stat(boot, &st) == 0 && S_ISREG(st.st_mode))
		for (size_t i = 0u; i < sizeof sizes / sizeof sizes[0]; i++)
			sized |= st.st_size == sizes[i];
	if (!sized) {
		snprintf(why, cap, "%s: not a floppy image of 360 KB to 2.88 MB", boot);
		return -1;
	}
	/* DOS's system and its FORMAT, by mtools: a floppy without them is
	 * not one this can install from. */
	{
		char *system[] = { "mdir", "-a", "-b", "-i", (char *)boot, "::IO.SYS", NULL };
		char *ibm[] = { "mdir", "-a", "-b", "-i", (char *)boot, "::IBMBIO.COM", NULL };
		char *format[] = { "mdir", "-b", "-i", (char *)boot, "::FORMAT.COM", NULL };

		if ((run_code(system) != 0 && run_code(ibm) != 0) || run_code(format) != 0) {
			snprintf(why, cap, "%s: not a DOS boot floppy with FORMAT.COM: no IO.SYS or "
			         "IBMBIO.COM, or no FORMAT.COM, at its root", boot);
			return -1;
		}
	}
	if (mkdir(dir, 0755) != 0) {
		snprintf(why, cap, "%s: %s%s", dir, strerror(errno),
		         errno == EEXIST ? ": setup makes a directory of its own" : "");
		return -1;
	}
	snprintf(floppy, sizeof floppy, "%s/setup.img", dir);
	snprintf(disk, sizeof disk, "%s/disk.img", dir);
	snprintf(stage, sizeof stage, "%s/setup", dir);
	if (mkdir(stage, 0755) != 0) {
		snprintf(why, cap, "%s: %s", stage, strerror(errno));
		goto done;
	}
	/* The user's floppy copied, theirs left as it was. */
	{
		char *copy[] = { "cp", (char *)boot, floppy, NULL };

		if (run(copy) != 0 || chmod(floppy, 0644) != 0) {
			snprintf(why, cap, "%s could not be copied", boot);
			goto done;
		}
	}
	if (put(stage, "HULLOFF.COM", msdos_off, 0644, why, cap) != 0 ||
	    put(stage, "HULLY.TXT", msdos_answers, 0644, why, cap) != 0 ||
	    put(stage, "AUTOEXEC.BAT", msdos_auto, 0644, why, cap) != 0 ||
	    put(stage, "HULLAUTO.BAT", msdos_disk_auto, 0644, why, cap) != 0)
		goto done;
	for (const char *const *f = (const char *const[]){ "HULLOFF.COM", "HULLY.TXT", "AUTOEXEC.BAT",
	                                                   "HULLAUTO.BAT", NULL }; *f != NULL; f++) {
		char  from[PATH_MAX + 32];
		char  to[64];
		char *copy[] = { "mcopy", "-o", "-i", floppy, from, to, NULL };

		snprintf(from, sizeof from, "%s/%s", stage, *f);
		snprintf(to, sizeof to, "::%s", *f);
		if (run(copy) != 0) {
			snprintf(why, cap, "mtools' mcopy could not put %s on the floppy: is there room?", *f);
			goto done;
		}
	}
	/* Whole cylinders of 16 heads and 63 sectors. */
	sectors = (uint32_t)((uint64_t)megabytes * 1024u * 1024u / 512u / (16u * 63u) * (16u * 63u));
	if ((fd = open(disk, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0 ||
	    ftruncate(fd, (off_t)sectors * 512) != 0) {
		snprintf(why, cap, "%s: %s", disk, strerror(errno));
		goto done;
	}
	if (msdos_table(fd, sectors, why, cap) != 0)
		goto done;
	if (close(fd) != 0) {
		fd = -1;
		snprintf(why, cap, "%s: %s", disk, strerror(errno));
		goto done;
	}
	fd = -1;
	if (put(dir, "install.conf",
	    "; Installs DOS onto disk.img from the floppy, unattended (hull setup\n"
	    "; msdos): its own FORMAT makes C: with the system, the floppy is\n"
	    "; copied into C:\\DOS, and it powers off. Its COM1 is the run's\n"
	    "; output. Then machine.conf boots the disk.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"msdos\")\n"
	    "  (source \"media:floppy\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"32M\")\n"
	    "  (media\n"
	    "    (medium (type floppy) (source \"image:./setup.img\"))\n"
	    "    (medium (type disk) (source \"image:./disk.img\"))))\n", 0644, why, cap) != 0 ||
	    put(dir, "machine.conf",
	    "; DOS as hull setup msdos installed it.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"msdos\")\n"
	    "  (source \"media:disk\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"32M\")\n"
	    "  (media\n"
	    "    (medium (type disk) (source \"image:./disk.img\"))))\n", 0644, why, cap) != 0)
		goto done;
	result = 0;
done:
	if (fd >= 0)
		close(fd);
	/* Refused, nothing of it left: what it made, by name, and DIR. */
	if (result != 0) {
		char path[PATH_MAX + 32];

		for (const char *const *f = (const char *const[]){ "setup/HULLOFF.COM", "setup/HULLY.TXT",
		                                                   "setup/AUTOEXEC.BAT", "setup/HULLAUTO.BAT",
		                                                   "setup.img", "disk.img", "install.conf",
		                                                   "machine.conf", NULL }; *f != NULL; f++) {
			snprintf(path, sizeof path, "%s/%s", dir, *f);
			unlink(path);
		}
		rmdir(stage);
		rmdir(dir);
	}
	return result;
}

/* --- macOS -------------------------------------------------------------- */

/* The holder's decision of 2026-11-24 (project.md section 9's macOS
 * entry): macOS on this machine, as the OSX-KVM projects boot it -- a q35
 * PC under OVMF, OpenCore its loader, Apple's SMC, a Penryn by Intel's
 * name -- so a project's Mac builds can be tested on hull, older releases
 * beside newer.
 *
 *   OpenCore.qcow2  OSX-KVM's OpenCore disk, pinned by commit and sha256;
 *                   its picker offers the recovery image and, once
 *                   installed, the system;
 *   recovery.img    a FAT disk holding com.apple.recovery.boot/, Apple's
 *                   BaseSystem.dmg and its chunklist, which OpenCore boots
 *                   as macrecovery's USB stick is booted -- no conversion,
 *                   so no dmg codec of qemu's is needed;
 *   macos.qcow2     the disk macOS is installed to, sparse;
 *   hull.conf       the machine: the installer is the user's to run on
 *                   its screen, macOS's setup having no answer file.
 *
 * The recovery image is Apple's, asked for as macrecovery asks: a session
 * from osrecovery.apple.com over https, then the version's board id, and
 * Apple's answer names the image and its chunklist. Its CDN has no
 * certificate for its own name, so both come over plain HTTP
 * (hull_https_plain_digested) and are checked as macrecovery checks them:
 * the chunklist's RSA signature against Apple's EFI ROM key, and each
 * chunk of the image against the chunklist's sha256 as it arrives, so
 * nothing unverified is kept. The answer's AH and CH lines are no
 * digests of the files -- they change with each request while the files
 * do not, measured -- and are not used. Apple's SMC key is not here and
 * never written by hull: (smc PATH) names the user's own file. */

#include <sys/random.h>

#include <openssl/bn.h>

#include "https.h"
#include "kernel_fetch.h"
#include "sha256.h"

#define OPENCORE_URL \
	"https://raw.githubusercontent.com/kholia/OSX-KVM/4c378a4b5e0b219783683012bec680325eb40719/" \
	"OpenCore/OpenCore.qcow2"
#define OPENCORE_SHA256 "6ed36c0c2a4206ccc695f6b1a734a1cc6f94d288b0517c705d351c63cb92a6f3"
#define RECOVERY_AGENT "User-Agent: InternetRecovery/1.0"

/* Each release Apple's recovery service serves, by the board id and MLB
 * OSX-KVM's fetch-macOS-v2.py asks with at the commit pinned above, and
 * whether it wants AVX2, which Ventura on requires and a Westmere lacks. */
static const struct mac_release {
	const char *name;
	const char *board;
	const char *mlb;
	const char *os;
	int         avx2;
} mac_releases[] = {
	{ "high-sierra", "Mac-7BA5B2D9E42DDD94", "00000000000J80300", "default", 0 },
	{ "mojave",      "Mac-7BA5B2DFE22DDD8C", "00000000000KXPG00", "default", 0 },
	{ "catalina",    "Mac-00BE6ED71E35EB86", "00000000000000000", "default", 0 },
	{ "big-sur",     "Mac-2BD1B31983FE1663", "00000000000000000", "default", 0 },
	{ "monterey",    "Mac-B809C3757DA9BB8D", "00000000000000000", "latest",  0 },
	{ "ventura",     "Mac-4B682C642B45593E", "00000000000000000", "latest",  1 },
	{ "sonoma",      "Mac-827FAC58A8FDFA22", "00000000000000000", "default", 1 },
	{ "sequoia",     "Mac-7BA5B2D9E42DDD94", "00000000000000000", "default", 1 },
	{ "tahoe",       "Mac-CFF7D910A743CAAF", "00000000000000000", "latest",  1 },
};

/* An answer's body, kept whole, at most 64 KiB. */
typedef struct recovery_answer {
	char   text[65536];
	size_t n;
} recovery_answer;

static int keep_answer(void *context, const uint8_t *data, size_t n, char *why, size_t cap)
{
	recovery_answer *a = context;

	if (n >= sizeof a->text - a->n) {
		snprintf(why, cap, "Apple's recovery service answered with more than 64 KiB");
		return -1;
	}
	memcpy(a->text + a->n, data, n);
	a->n += n;
	a->text[a->n] = '\0';
	return 0;
}

/* `n` random bytes as upper-case hex into `out`. */
static int random_hex(char *out, size_t n)
{
	uint8_t bytes[32];

	if (n > sizeof bytes || getrandom(bytes, n, 0) != (ssize_t)n)
		return -1;
	for (size_t i = 0u; i < n; i++)
		sprintf(out + 2u * i, "%02X", bytes[i]);
	return 0;
}

/* The value of the answer's line `key: value`, into `out`. */
static int answer_line(const char *text, const char *key, char *out, size_t cap)
{
	size_t n = strlen(key);

	for (const char *line = text; line != NULL && *line != '\0';
	     line = strchr(line, '\n') != NULL ? strchr(line, '\n') + 1 : NULL) {
		size_t len = strcspn(line, "\r\n");

		if (len > n + 2u && strncmp(line, key, n) == 0 && line[n] == ':' && line[n + 1u] == ' ' &&
		    len - n - 2u < cap) {
			memcpy(out, line + n + 2u, len - n - 2u);
			out[len - n - 2u] = '\0';
			return 0;
		}
	}
	return -1;
}

/* Apple's EFI ROM public key, RSA-2048 with exponent 65537, which signs a
 * recovery image's chunklist: public, as macrecovery and OSX-KVM carry it. */
static const char apple_rom_key[] =
    "C3E748CAD9CD384329E10E25A91E43E1A762FF529ADE578C935BDDF9B13F2179D4855E6FC89E9E29CA1251"
    "7D17DFA1EDCE0BEBF0EA7B461FFE61D94E2BDF72C196F89ACD3536B644064014DAE25A15DB6BB0852ECBD1"
    "20916318D1CCDEA3C84C92ED743FC176D0BACA920D3FCF3158AFF731F88CE0623182A8ED67E650515F7574"
    "5909F07D415F55FC15A35654D118C55A462D37A3ACDA08612F3F3F6571761EFCCBCC299AEE99B3A4FD6212"
    "CCFFF5EF37A2C334E871191F7E1C31960E010A54E86FA3F62E6D6905E1CD57732410A3EB0C6B4DEFDABE9F"
    "59BF1618758C751CD56CEF851D1C0EAA1C558E37AC108DA9089863D20E2E7E4BF475EC66FE6B3EFDCF";

/* A chunklist, `CNKL`: a 0x24-byte header, then each chunk's size and
 * sha256, then Apple's signature over both. */
#define CHUNKLIST_HEADER 0x24u
#define CHUNK_ENTRY      0x24u
#define CHUNKS_MAX       65536u

/* Whether the chunklist `list` of `n` bytes is Apple's: its header and
 * entries hashed, and the signature after them, a little-endian RSA
 * number, raised to 65537 modulo Apple's key, PKCS #1 v1.5's padding of
 * that sha256. Its chunk count into `chunks`. 0, or -1 with `why`. */
static int chunklist_signed(const uint8_t *list, size_t n, uint64_t *chunks, char *why, size_t cap)
{
	static const uint8_t prefix[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
	                                  0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
	uint8_t     expected[256];
	uint8_t     digest[32];
	uint8_t     got[256];
	hull_sha256 hash;
	uint64_t    count, offset, signature;
	BIGNUM     *key = NULL, *e = NULL, *sig = NULL, *out = NULL;
	BN_CTX     *ctx = NULL;
	int         good = 0;

	if (n < CHUNKLIST_HEADER || memcmp(list, "CNKL", 4u) != 0 || list[8] != 1u || list[9] != 1u) {
		snprintf(why, cap, "the chunklist is not one");
		return -1;
	}
	memcpy(&count, list + 12, 8u);
	memcpy(&offset, list + 20, 8u);
	memcpy(&signature, list + 28, 8u);
	if (list[10] != 1u || count == 0u || count > CHUNKS_MAX || offset != CHUNKLIST_HEADER ||
	    signature != offset + CHUNK_ENTRY * count || n < signature + 256u) {
		snprintf(why, cap, "the chunklist is not one Apple signs with its ROM key");
		return -1;
	}
	hull_sha256_init(&hash);
	hull_sha256_update(&hash, list, (size_t)signature);
	hull_sha256_final(&hash, digest);
	/* 0x00 0x01, 0xff to fill, 0x00, the DigestInfo, the digest. */
	memset(expected, 0xff, sizeof expected);
	expected[0] = 0x00;
	expected[1] = 0x01;
	expected[256 - 32 - sizeof prefix - 1] = 0x00;
	memcpy(expected + 256 - 32 - sizeof prefix, prefix, sizeof prefix);
	memcpy(expected + 256 - 32, digest, 32u);
	if ((ctx = BN_CTX_new()) != NULL && BN_hex2bn(&key, apple_rom_key) > 0 &&
	    BN_dec2bn(&e, "65537") > 0 && (sig = BN_lebin2bn(list + signature, 256, NULL)) != NULL &&
	    (out = BN_new()) != NULL && BN_mod_exp(out, sig, e, key, ctx) == 1 &&
	    BN_bn2binpad(out, got, sizeof got) == (int)sizeof got)
		good = memcmp(got, expected, sizeof got) == 0;
	BN_free(key);
	BN_free(e);
	BN_free(sig);
	BN_free(out);
	BN_CTX_free(ctx);
	if (!good) {
		snprintf(why, cap, "the chunklist's signature is not Apple's");
		return -1;
	}
	*chunks = count;
	return 0;
}

/* An image streamed to its file, each chunk held to the chunklist's
 * sha256 as it ends. */
typedef struct chunked {
	int            fd;
	const uint8_t *entries;   /* the chunklist's, each size and sha256 */
	uint64_t       count;
	uint64_t       at;        /* the chunk being read */
	uint32_t       left;      /* its bytes still to come */
	hull_sha256    hash;
} chunked;

static int next_chunk(chunked *c)
{
	if (c->at >= c->count)
		return -1;
	memcpy(&c->left, c->entries + CHUNK_ENTRY * c->at, 4u);
	hull_sha256_init(&c->hash);
	return 0;
}

static int write_chunked(void *context, const uint8_t *data, size_t n, char *why, size_t cap)
{
	chunked *c = context;

	while (n > 0u) {
		size_t  take;
		ssize_t put;

		if (c->left == 0u && next_chunk(c) != 0) {
			snprintf(why, cap, "the image is longer than its chunklist says");
			return -1;
		}
		take = n < c->left ? n : c->left;
		hull_sha256_update(&c->hash, data, take);
		for (size_t done = 0u; done < take; done += (size_t)put) {
			put = write(c->fd, data + done, take - done);
			if (put < 0 && errno == EINTR) {
				put = 0;
				continue;
			}
			if (put <= 0) {
				snprintf(why, cap, "the image could not be written: %s", strerror(errno));
				return -1;
			}
		}
		data += take;
		n -= take;
		c->left -= (uint32_t)take;
		if (c->left == 0u) {
			uint8_t digest[32];

			hull_sha256_final(&c->hash, digest);
			if (memcmp(digest, c->entries + CHUNK_ENTRY * c->at + 4u, 32u) != 0) {
				snprintf(why, cap, "the image's chunk %llu is not the chunklist's",
				         (unsigned long long)c->at + 1u);
				return -1;
			}
			c->at++;
		}
	}
	return 0;
}

/* GET `url` with Apple's asset `token`, plain HTTP being taken because
 * what comes is checked: the body to `body`. */
static int fetch_checked(const char *url, const char *token, hull_https_body body, void *context,
                         char *why, size_t cap)
{
	char                cookie[2100];
	const char         *headers[] = { RECOVERY_AGENT, cookie, NULL };
	hull_https_response got = { 0 };
	int                 asked;

	snprintf(cookie, sizeof cookie, "Cookie: AssetToken=%s", token);
	hull_https_plain_digested(1);
	asked = hull_https_get(url, headers, &got, body, context, why, cap);
	hull_https_plain_digested(0);
	if (asked == 0 && got.status != 200) {
		snprintf(why, cap, "%s answered %d", url, got.status);
		return -1;
	}
	return asked;
}

/* Whether this host's processors have AVX2. */
static int host_has_avx2(void)
{
	FILE *in = fopen("/proc/cpuinfo", "re");
	char  line[8192];
	int   has = 0;

	while (in != NULL && !has && fgets(line, sizeof line, in) != NULL)
		if (strncmp(line, "flags", 5u) == 0)
			has = strstr(line, " avx2") != NULL;
	if (in != NULL)
		fclose(in);
	return has;
}

/* Apple's recovery image for `release` into DIR as BaseSystem.dmg and
 * BaseSystem.chunklist, as macrecovery fetches it. 0, or -1 with `why`. */
static int fetch_recovery(const struct mac_release *release, const char *dir, char *why,
                          size_t cap)
{
	static recovery_answer said;
	const char         *agent[] = { RECOVERY_AGENT, NULL };
	hull_https_response got = { 0 };
	char                cid[17], k[65], fg[65];
	char                body[512];
	char                cookie[1100];
	const char         *ask[] = { RECOVERY_AGENT, cookie, "Content-Type: text/plain", NULL };
	hull_https_upload   up = { 0 };
	char                image[2048], image_token[2048];
	char                list[2048], list_token[2048];
	char                path[PATH_MAX];
	static uint8_t      entries[CHUNK_ENTRY * CHUNKS_MAX];
	chunked             chunks = { 0 };
	int                 fd;
	int                 status;

	said.n = 0u;
	if (hull_https_get("https://osrecovery.apple.com/", agent, &got, keep_answer, &said, why,
	                   cap) != 0)
		return -1;
	if (got.cookie[0] == '\0') {
		snprintf(why, cap, "Apple's recovery service gave no session (answered %d)", got.status);
		return -1;
	}
	snprintf(cookie, sizeof cookie, "Cookie: %s", got.cookie);
	if (random_hex(cid, 8u) != 0 || random_hex(k, 32u) != 0 || random_hex(fg, 32u) != 0) {
		snprintf(why, cap, "no random bytes for the request");
		return -1;
	}
	snprintf(body, sizeof body, "cid=%s\nsn=%s\nbid=%s\nk=%s\nfg=%s\nos=%s", cid, release->mlb,
	         release->board, k, fg, release->os);
	up.data = (const uint8_t *)body;
	up.length = (long long)strlen(body);
	memset(&got, 0, sizeof got);
	said.n = 0u;
	if (hull_https_request("POST", "https://osrecovery.apple.com/InstallationPayload/RecoveryImage",
	                       ask, &up, &got, keep_answer, &said, why, cap) != 0)
		return -1;
	if (got.status != 200 || answer_line(said.text, "AU", image, sizeof image) != 0 ||
	    answer_line(said.text, "AT", image_token, sizeof image_token) != 0 ||
	    answer_line(said.text, "CU", list, sizeof list) != 0 ||
	    answer_line(said.text, "CT", list_token, sizeof list_token) != 0) {
		snprintf(why, cap, "Apple's recovery service named no image for %s (answered %d)",
		         release->name, got.status);
		return -1;
	}
	/* The chunklist first, Apple's signature on it checked; then the
	 * image, each of its chunks held to the chunklist as it comes. */
	said.n = 0u;
	if (fetch_checked(list, list_token, keep_answer, &said, why, cap) != 0 ||
	    chunklist_signed((const uint8_t *)said.text, said.n, &chunks.count, why, cap) != 0)
		return -1;
	memcpy(entries, said.text + CHUNKLIST_HEADER, (size_t)(CHUNK_ENTRY * chunks.count));
	chunks.entries = entries;
	snprintf(path, sizeof path, "%s/BaseSystem.chunklist", dir);
	if ((fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0 ||
	    write(fd, said.text, said.n) != (ssize_t)said.n || close(fd) != 0) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	fprintf(stderr, "hull: fetching macOS %s's recovery image from Apple, about 700 MB, each "
	        "chunk checked against its signed chunklist: %s\n", release->name, image);
	snprintf(path, sizeof path, "%s/BaseSystem.dmg", dir);
	if ((chunks.fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644)) < 0) {
		snprintf(why, cap, "%s: %s", path, strerror(errno));
		return -1;
	}
	status = fetch_checked(image, image_token, write_chunked, &chunks, why, cap);
	close(chunks.fd);
	if (status == 0 && (chunks.at != chunks.count || chunks.left != 0u)) {
		snprintf(why, cap, "the image ended at chunk %llu of %llu",
		         (unsigned long long)chunks.at, (unsigned long long)chunks.count);
		status = -1;
	}
	if (status != 0)
		unlink(path);
	return status;
}

int hull_setup_macos(const char *version, const char *dir, const char *osk, unsigned gigabytes,
                     char *why, size_t cap)
{
	const struct mac_release *release = NULL;
	char                      key[PATH_MAX];
	char                      path[PATH_MAX + 64];
	char                      dmg[PATH_MAX + 64], list[PATH_MAX + 64];
	char                      sectors[32], size[32];
	char                     *spec = NULL;
	struct stat               st;
	int                       result = -1;

	for (size_t i = 0u; i < sizeof mac_releases / sizeof mac_releases[0]; i++)
		if (strcmp(mac_releases[i].name, version) == 0)
			release = &mac_releases[i];
	if (release == NULL) {
		snprintf(why, cap, "%s is no release of macOS this knows: high-sierra, mojave, catalina, "
		         "big-sur, monterey, ventura, sonoma, sequoia or tahoe", version);
		return -1;
	}
	if (release->avx2 && !host_has_avx2()) {
		snprintf(why, cap, "macOS %s needs AVX2, from Ventura on, and this host's processors "
		         "have none: Monterey is the newest that runs here", version);
		return -1;
	}
	if (realpath(osk, key) == NULL || stat(key, &st) != 0 || !S_ISREG(st.st_mode) ||
	    strpbrk(key, "\"\\\n") != NULL) {
		snprintf(why, cap, "%s: Apple's SMC key, your own, is read from this file as the "
		         "machine starts, and there is no such file", osk);
		return -1;
	}
	if (mkdir(dir, 0755) != 0) {
		snprintf(why, cap, "%s: %s", dir, strerror(errno));
		return -1;
	}
	snprintf(path, sizeof path, "%s/OpenCore.qcow2", dir);
	fprintf(stderr, "hull: fetching OpenCore, OSX-KVM's, 19 MB: %s\n", OPENCORE_URL);
	if (hull_fetch_file(OPENCORE_URL, OPENCORE_SHA256, path, why, cap) != 0 ||
	    chmod(path, 0644) != 0 || fetch_recovery(release, dir, why, cap) != 0)
		goto done;
	/* The recovery stick: FAT32 with no table, a little over what it
	 * holds, made and filled by mtools as the user. */
	snprintf(dmg, sizeof dmg, "%s/BaseSystem.dmg", dir);
	snprintf(list, sizeof list, "%s/BaseSystem.chunklist", dir);
	if (stat(dmg, &st) != 0) {
		snprintf(why, cap, "%s: %s", dmg, strerror(errno));
		goto done;
	}
	snprintf(sectors, sizeof sectors, "%lld",
	         ((long long)st.st_size / 512 + 2 * 65536 + 2047) / 2048 * 2048);
	snprintf(path, sizeof path, "%s/recovery.img", dir);
	{
		char *format[] = { "mformat", "-C", "-F", "-T", sectors, "-v", "RECOVERY", "-i", path,
		                   "::", NULL };
		char *folder[] = { "mmd", "-i", path, "::com.apple.recovery.boot", NULL };
		char *copy[] = { "mcopy", "-i", path, dmg, list, "::com.apple.recovery.boot/", NULL };

		if (run(format) != 0 || run(folder) != 0 || run(copy) != 0) {
			snprintf(why, cap, "the recovery disk could not be made with mtools (mformat, mmd, "
			         "mcopy): is the mtools package installed?");
			goto done;
		}
	}
	unlink(dmg);
	unlink(list);
	snprintf(path, sizeof path, "%s/macos.qcow2", dir);
	snprintf(size, sizeof size, "%uG", gigabytes);
	{
		char *make[] = { "qemu-img", "create", "-q", "-f", "qcow2", path, size, NULL };

		if (run(make) != 0) {
			snprintf(why, cap, "qemu-img could not make %s", path);
			goto done;
		}
	}
	if (asprintf(&spec,
	    "; macOS %s's installer, as hull setup macos made it. Its screen is\n"
	    "; hull-gui's (Screen): in OpenCore's picker press Space and choose\n"
	    "; RECOVERY (dmg); then Disk Utility erases the %u GB disk as APFS and\n"
	    "; Reinstall macOS installs onto it. After, the picker boots the disk,\n"
	    "; and recovery.img may leave (media ...). Apple's SMC key is read\n"
	    "; from your own file as the machine starts.\n"
	    "(machine\n"
	    "  (backend pc)\n"
	    "  (hostname \"macos-%s\")\n"
	    "  (source \"media:disk\")\n"
	    "  (mutable direct)\n"
	    "  (memory \"4G\")\n"
	    "  (cpus 4)\n"
	    "  (network nat)\n"
	    "  (display local)\n"
	    "  (media\n"
	    "    (medium (type disk) (source \"image:./OpenCore.qcow2\"))\n"
	    "    (medium (type disk) (source \"image:./recovery.img\"))\n"
	    "    (medium (type disk) (source \"image:./macos.qcow2\")))\n"
	    "  (tuning (pc (machine q35) (firmware uefi) (cpu penryn) (vga vmware) (nic vmxnet3)\n"
	    "              (pointer tablet) (smc \"%s\"))))\n",
	    release->name, gigabytes, release->name, key) < 0) {
		spec = NULL;
		snprintf(why, cap, "out of memory");
		goto done;
	}
	if (put(dir, "hull.conf", spec, 0644, why, cap) != 0)
		goto done;
	result = 0;
done:
	/* A setup that failed leaves nothing: what it made, by name, and the
	 * folder it made, so it can be run again. */
	if (result != 0) {
		static const char *const made[] = { "OpenCore.qcow2", "BaseSystem.chunklist",
		                                     "BaseSystem.dmg", "recovery.img", "macos.qcow2",
		                                     "hull.conf" };

		for (size_t i = 0u; i < sizeof made / sizeof made[0]; i++) {
			snprintf(path, sizeof path, "%s/%s", dir, made[i]);
			unlink(path);
		}
		rmdir(dir);
	}
	free(spec);
	return result;
}
