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
 *   password        the account's, which the answer file must carry for
 *                   its one automatic logon, 0600.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int hull_setup_windows(const char *iso, const char *dir, unsigned index, const char *user,
                       char *why, size_t cap)
{
	char        disc[PATH_MAX];
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
	if (mkdir(stage, 0700) != 0) {
		snprintf(why, cap, "%s: %s", stage, strerror(errno));
		goto done;
	}
	if (put(stage, "autounattend.xml", xml, 0600, why, cap) != 0 ||
	    put(stage, "hull-setup.ps1", script, 0600, why, cap) != 0 ||
	    put(stage, "authorized_keys", keys, 0600, why, cap) != 0)
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
	 * their long names. */
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
		                                                   "authorized_keys", NULL }; *f != NULL; f++) {
			char  from[PATH_MAX + 32];
			char *copy[] = { "mcopy", "-i", image, from, "::/", NULL };

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
	if (put(dir, "machine.conf",
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
	    "              (state \"./\") (nic e1000e))))\n", 0644, why, cap) != 0)
		goto done;
	result = 0;
done:
	free(keys);
	free(xml);
	free(spec);
	return result;
}
