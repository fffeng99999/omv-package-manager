<?php

/**
 * This file is part of OpenMediaVault.
 *
 * @license   https://www.gnu.org/licenses/gpl.html GPL Version 3
 * @author    ${GITHUB_USER}
 *
 * OpenMediaVault is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * OpenMediaVault is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

namespace Engined\Rpc;

/**
 * Package manager RPC service (inspired by OpenWrt's luci-app-package-manager).
 *
 * NOTE: The class name and RPC service name must not collide with the
 * core "PluginMgmt"/"Plugin" service, therefore we use "PkgMgr".
 */
class PkgMgr extends \OMV\Rpc\ServiceAbstract
{
    public function getName()
    {
        return "PkgMgr";
    }

    public function initialize()
    {
        $this->registerMethod("getInstalledList");
        $this->registerMethod("getInstalledListBg");
        $this->registerMethod("getAvailableList");
        $this->registerMethod("getAvailableListBg");
        $this->registerMethod("getUpgradableList");
        $this->registerMethod("getUpgradableListBg");
        $this->registerMethod("installPkg");
        $this->registerMethod("installDeb");
        $this->registerMethod("removePkg");
        $this->registerMethod("simulate");
        $this->registerMethod("preview");
        $this->registerMethod("updateLists");
        $this->registerMethod("upgradeAll");
        $this->registerMethod("autoremove");
        $this->registerMethod("getDebInfo");
        $this->registerMethod("getPkgDetails");
        $this->registerMethod("setHold");
    }

    /**
     * Get the list of installed packages.
     * @param params Standard 'getlist' parameters (start/limit/sortfield/
     *   sortdir/search).
     * @return An array containing the fields 'total' and 'data'.
     */
    public function getInstalledList($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.common.getlist");
        // Note: The format string must be wrapped in single quotes for the
        // shell (see \OMV\ProductInfo::getVersion), otherwise '${Package}'
        // would be expanded by the shell. The '\t' and '\n' escape sequences
        // are interpreted by dpkg itself, so they must survive as literals
        // (PHP single quoted string).
        $cmd = new \OMV\System\Process("dpkg-query", [
            '-W',
            '-f=\'${Package}\t${Version}\t${Architecture}\t${Installed-Size}\t${Status}\t${binary:Summary}\n\''
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        if (0 !== $exitStatus) {
            throw new \OMV\ExecException($cmd, $output, $exitStatus);
        }
        // Get the list of packages that are held back ('apt-mark showhold').
        $held = [];
        $holdCmd = new \OMV\System\Process("apt-mark", ["showhold"]);
        $holdCmd->setQuiet(true);
        $holdCmd->execute($holdOutput, $holdStatus);
        if (0 === $holdStatus) {
            foreach ($holdOutput as $name) {
                $name = trim($name);
                if ("" !== $name) {
                    $held[$name] = true;
                }
            }
        }
        $objects = [];
        foreach ($output as $line) {
            $line = rtrim($line, "\r\n");
            $fields = explode("\t", $line);
            if (6 !== count($fields)) {
                continue;
            }
            // Only really installed packages.
            if ("install ok installed" !== $fields[4]) {
                continue;
            }
            $objects[] = [
                "package" => $fields[0],
                "version" => $fields[1],
                "architecture" => $fields[2],
                "size" => intval($fields[3]) * 1024,
                "hold" => isset($held[$fields[0]]),
                "description" => $fields[5]
            ];
        }
        return $this->applyFilter(
            $objects,
            $params['start'],
            $params['limit'],
            $params['sortfield'],
            $params['sortdir'],
            $params['search']
        );
    }

    public function getInstalledListBg($params, $context)
    {
        return $this->callMethodBg("getInstalledList", $params, $context);
    }

    /**
     * Get the list of packages available from the configured repositories.
     * @return An array containing the fields 'total' and 'data'.
     */
    public function getAvailableList($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.common.getlist");
        $objects = $this->getAvailableRecords();
        return $this->applyFilter(
            $objects,
            $params['start'],
            $params['limit'],
            $params['sortfield'],
            $params['sortdir'],
            $params['search']
        );
    }

    public function getAvailableListBg($params, $context)
    {
        return $this->callMethodBg("getAvailableList", $params, $context);
    }

    /**
     * Get the list of packages that can be upgraded.
     * @return An array containing the fields 'total' and 'data'.
     */
    public function getUpgradableList($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.common.getlist");
        $cmd = new \OMV\System\Process("apt-get", [
            "--quiet", "--simulate", "--yes", "upgrade"
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        if (0 !== $exitStatus) {
            throw new \OMV\ExecException($cmd, $output, $exitStatus);
        }
        $objects = [];
        foreach ($output as $line) {
            // Upgradable: Inst curl [7.88.1-10+deb12u5] (8.5.0-1 Debian:trixie [amd64])
            // New:        Inst foo (1.2.3 Debian:trixie [amd64])
            if (1 === preg_match(
                "/^Inst (\S+)(?: \[(.*?)\])? \((\S+)[^)]*?\[(.*?)\]/",
                rtrim($line, "\r\n"), $matches
            )) {
                $objects[] = [
                    "package" => $matches[1],
                    "architecture" => $matches[4],
                    "current" => $matches[2] ?? "",
                    "new" => $matches[3]
                ];
            }
        }
        return $this->applyFilter(
            $objects,
            $params['start'],
            $params['limit'],
            $params['sortfield'],
            $params['sortdir'],
            $params['search']
        );
    }

    public function getUpgradableListBg($params, $context)
    {
        return $this->callMethodBg("getUpgradableList", $params, $context);
    }

    /**
     * Install one or more packages from the repositories.
     * @param params An array containing the following fields:
     *   \em packages An array of package names to install.
     *   \em force Skip the 'openmediavault' removal protection (optional).
     * @return The name of the background process status file.
     */
    public function installPkg($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.pluginmgr.installpkg");
        $params['packages'] = self::normalizePackages($params['packages']);
        \OMV\System\Apt::assertNotLocked();
        $args = array_merge(
            ["--yes", "install"],
            array_map("escapeshellarg", $params['packages'])
        );
        // Safety net: refuse if the operation would remove openmediavault
        // packages, unless 'force' is given explicitly.
        $this->assertNoOmvRemoval($args, $params['force'] ?? false);
        return $this->executeApt($args);
    }

    /**
     * Install a local .deb file located inside a shared folder.
     * @param params An array containing the following fields:
     *   \em sharedfolderref The UUID of the shared folder.
     *   \em debpath The path below the shared folder root (may be empty).
     *   \em filename The name of the .deb file.
     *   \em force Skip the 'openmediavault' removal protection (optional).
     * @return The name of the background process status file.
     */
    public function installDeb($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.pluginmgr.installedb");
        \OMV\System\Apt::assertNotLocked();
        $path = $this->validateDebPath($params, $context);
        $args = ["--yes", "install", escapeshellarg($path)];
        $this->assertNoOmvRemoval($args, $params['force'] ?? false);
        return $this->executeApt($args);
    }

    /**
     * Remove one or more packages.
     * @param params An array containing the following fields:
     *   \em packages An array of package names to remove.
     *   \em force Allow the removal even if it affects openmediavault
     *     packages (double confirmation on the web interface required).
     * @return The name of the background process status file.
     */
    public function removePkg($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.pluginmgr.removepkg");
        $params['packages'] = self::normalizePackages($params['packages']);
        \OMV\System\Apt::assertNotLocked();
        $args = array_merge(
            ["--yes", ($params['purge'] ?? false) ? "purge" : "remove"],
            array_map("escapeshellarg", $params['packages'])
        );
        // Hard protection for the OpenMediaVault installation itself.
        if (!($params['force'] ?? false)) {
            foreach ($params['packages'] as $package) {
                if (0 === strpos($package, "openmediavault")) {
                    throw new \OMV\Exception(
                        "Refusing to remove package '%s': packages starting ".
                        "with 'openmediavault' would break the system. Use ".
                        "the forced removal only if you really know what you ".
                        "are doing.",
                        $package
                    );
                }
            }
            $this->assertNoOmvRemoval($args, false);
        }
        return $this->executeApt($args);
    }

    /**
     * Simulate an install/remove operation and return the expected
     * changes. Used by the confirmation dialog of the web interface.
     * @param params An array containing the following fields:
     *   \em packages An array of package names.
     *   \em action Either 'install' or 'remove'.
     * @return An array with the fields 'install', 'remove' (arrays of
     *   package names) and 'removesOmv' (boolean).
     */
    public function simulate($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.pluginmgr.simulate");
        $params['packages'] = self::normalizePackages($params['packages']);
        $result = $this->runSimulation(array_merge(
            [$params['action']],
            array_map("escapeshellarg", $params['packages'])
        ));
        $result['removesOmv'] = self::hasOmvRemoval($result['remove']);
        return $result;
    }

    /**
     * Run 'apt-get --simulate' as a background task so the raw output
     * (the packages that would be installed/removed/upgraded) can be
     * shown in a task dialog before the real operation is executed.
     * This mirrors the dependency preview of the OpenWrt package manager.
     * @param params An array containing the fields 'packages' (array of
     *   package names) and 'action' ('install'|'remove'|'upgrade').
     * @return The name of the background process status file.
     */
    public function preview($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.pluginmgr.simulate");
        $params['packages'] = self::normalizePackages($params['packages']);
        $args = array_merge(
            ["--quiet", "--simulate", "--yes"],
            [$params['action']],
            array_map("escapeshellarg", $params['packages'])
        );
        return $this->executeApt($args);
    }

    /**
     * Update the package index files ('apt-get update').
     * @return The name of the background process status file.
     */
    public function updateLists($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        \OMV\System\Apt::assertNotLocked();
        return $this->executeApt(["update"]);
    }

    /**
     * Upgrade all installed packages ('apt-get --yes upgrade').
     * @param params An array containing the optional field 'force' to skip
     *   the 'openmediavault' removal protection.
     * @return The name of the background process status file.
     */
    public function upgradeAll($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        \OMV\System\Apt::assertNotLocked();
        $params = is_array($params) ? $params : [];
        $args = ["--yes", "upgrade"];
        $this->assertNoOmvRemoval($args, $params['force'] ?? false);
        return $this->executeApt($args);
    }

    /**
     * Remove automatically installed packages that are no longer required
     * ('apt-get --yes autoremove').
     * @param params An array containing the optional field 'force' to skip
     *   the 'openmediavault' removal protection.
     * @return The name of the background process status file.
     */
    public function autoremove($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        \OMV\System\Apt::assertNotLocked();
        $params = is_array($params) ? $params : [];
        $args = ["--yes", "autoremove"];
        $this->assertNoOmvRemoval($args, $params['force'] ?? false);
        return $this->executeApt($args);
    }

    /**
     * Mark a package as held ('apt-mark hold') or not held
     * ('apt-mark unhold'). Held packages are not upgraded automatically.
     * @param params An array containing the fields 'package' (string) and
     *   'hold' (boolean).
     * @return An associative array with the field 'output'.
     */
    public function setHold($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.pluginmgr.sethold");
        $cmd = new \OMV\System\Process("apt-mark", [
            ($params['hold'] ? "hold" : "unhold"),
            escapeshellarg($params['package'])
        ]);
        $cmd->setQuiet(true);
        $cmd->setRedirect2to1();
        $cmd->execute($output, $exitStatus);
        if (0 !== $exitStatus) {
            throw new \OMV\ExecException($cmd, $output, $exitStatus);
        }
        return ["output" => implode("\n", $output)];
    }

    /**
     * Read the meta data of a local .deb file inside a shared folder.
     * @param params Same fields as installDeb (without 'force').
     * @return An associative array with the dpkg-deb fields.
     */
    public function getDebInfo($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.pluginmgr.installedb");
        $path = $this->validateDebPath($params, $context);
        $cmd = new \OMV\System\Process("dpkg-deb", [
            "--field",
            escapeshellarg($path),
            "Package", "Version", "Architecture", "Maintainer",
            "Installed-Size", "Description"
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        if (0 !== $exitStatus) {
            throw new \OMV\ExecException($cmd, $output, $exitStatus);
        }
        $info = [];
        foreach ($output as $line) {
            if (1 === preg_match("/^(\S+):\s*(.*)$/", $line, $matches)) {
                $info[strtolower($matches[1])] = trim($matches[2]);
            }
        }
        return $info;
    }

    /**
     * Get the details of a package (installed and/or available in the
     * configured repositories). Used by the package details page.
     * @param params An array containing the field 'package'.
     * @return An associative array with the package meta data.
     */
    public function getPkgDetails($params, $context)
    {
        $this->validateMethodContext($context, [
            "role" => OMV_ROLE_ADMINISTRATOR
        ]);
        $this->validateMethodParams($params, "rpc.pluginmgr.pkgdetails");
        $package = $params['package'];
        // Package names must be safe: Debian policy allows lowercase
        // letters, digits, '+', '.', '-' and '_'.
        if (1 !== preg_match("/^[a-z0-9+._-]+$/", $package)) {
            throw new \OMV\Exception(
                "Invalid package name '%s'.", $package
            );
        }
        $result = [
            "package" => $package,
            "version" => "",
            "architecture" => "",
            "maintainer" => "",
            "homepage" => "",
            "section" => "",
            "priority" => "",
            "installedSize" => "",
            "downloadSize" => "",
            "depends" => "",
            "preDepends" => "",
            "recommends" => "",
            "suggests" => "",
            "breaks" => "",
            "replaces" => "",
            "description" => "",
            "installed" => false,
            "installedVersion" => "",
            "hold" => false,
            "candidate" => "",
            "origin" => "",
            "status" => "Not installed",
            "installedSizeText" => "",
            "downloadSizeText" => ""
        ];
        // Parse the first record of 'apt-cache show <package>'. Note:
        // every command uses its own output variable because PHP's exec()
        // (used by \OMV\System\Process::execute) appends to the array
        // instead of clearing it.
        $cmd = new \OMV\System\Process("apt-cache", [
            "show", escapeshellarg($package)
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($showOutput, $exitStatus);
        if (0 === $exitStatus) {
            $fields = $this->parseDebianControl(implode("\n", $showOutput));
            $map = [
                "version" => "version",
                "architecture" => "architecture",
                "maintainer" => "maintainer",
                "homepage" => "homepage",
                "section" => "section",
                "priority" => "priority",
                "depends" => "depends",
                "pre-depends" => "preDepends",
                "recommends" => "recommends",
                "suggests" => "suggests",
                "breaks" => "breaks",
                "replaces" => "replaces"
            ];
            foreach ($map as $key => $prop) {
                if (isset($fields[$key])) {
                    $result[$prop] = $fields[$key];
                }
            }
            if (isset($fields['installed-size'])) {
                $result['installedSize'] = intval($fields['installed-size']) * 1024;
            }
            if (isset($fields['size'])) {
                $result['downloadSize'] = intval($fields['size']);
            }
            if (isset($fields['description'])) {
                $result['description'] = $fields['description'];
            }
        }
        // Installed status and version. Note: the format string must be a
        // PHP single-quoted string with literal shell quotes, otherwise
        // '${Version}' would be interpolated by PHP (see getInstalledList).
        $cmd = new \OMV\System\Process("dpkg-query", [
            "-W",
            '-f=\'${Version}\t${Status}\n\'',
            escapeshellarg($package)
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($queryOutput, $exitStatus);
        if ((0 === $exitStatus) && !empty($queryOutput) &&
            (2 === count($fields = explode("\t", rtrim($queryOutput[0], "\r\n"))))) {
            if ("install ok installed" === $fields[1]) {
                $result['installed'] = true;
                $result['installedVersion'] = $fields[0];
            }
        }
        // Hold status.
        $cmd = new \OMV\System\Process("apt-mark", [
            "showhold", escapeshellarg($package)
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($holdOutput, $exitStatus);
        if ((0 === $exitStatus) && (trim($holdOutput[0] ?? "") === $package)) {
            $result['hold'] = true;
        }
        // Candidate version and origin ('apt-cache policy').
        $cmd = new \OMV\System\Process("apt-cache", [
            "policy", escapeshellarg($package)
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($policyOutput, $exitStatus);
        if (0 === $exitStatus) {
            foreach ($policyOutput as $line) {
                $line = rtrim($line, "\r\n");
                if (1 === preg_match("/^\s*Candidate:\s*(.+)$/", $line, $m)) {
                    $result['candidate'] = trim($m[1]);
                } elseif (("" === $result['origin']) &&
                    1 === preg_match("/^\s*\d+\s+(\S+)\s/", $line, $m) &&
                    (0 !== strpos($m[1], "/"))) {
                    // First repository URI of the version table.
                    $result['origin'] = trim($m[1]);
                }
            }
        }
        // Pre-computed display strings. They are untranslated on purpose:
        // the workbench templates pipe them through the 'translate' filter,
        // which uses the plugin translation catalog of the selected UI
        // language.
        if ($result['installed']) {
            $result['status'] =
                (("" !== $result['candidate']) &&
                 ("(none)" !== $result['candidate']) &&
                 ($result['candidate'] !== $result['installedVersion']))
                    ? "Upgradable" : "Installed";
        }
        if (is_int($result['installedSize'])) {
            $result['installedSizeText'] = self::formatSize($result['installedSize']);
        }
        if (is_int($result['downloadSize'])) {
            $result['downloadSizeText'] = self::formatSize($result['downloadSize']);
        }
        return $result;
    }

    /**
     * Format a byte count as a human readable string (SI units are
     * language independent, so this does not need translation).
     * @param bytes The number of bytes.
     * @return The formatted string, e.g. '1.5 MiB'.
     */
    private static function formatSize($bytes)
    {
        $units = ["B", "KiB", "MiB", "GiB", "TiB"];
        $i = 0;
        $value = floatval($bytes);
        while (($value >= 1024) && ($i < count($units) - 1)) {
            $value /= 1024;
            $i++;
        }
        return sprintf(
            ((0 === $i) ? "%d %s" : "%.1f %s"),
            $value,
            $units[$i]
        );
    }

    /**
     * Parse the fields of a Debian control style record (the first
     * paragraph of the output). Multi-line values (e.g. Description)
     * are joined with a newline.
     * @param text The control record text.
     * @return An associative array with lowercased field names.
     */
    private function parseDebianControl($text)
    {
        $fields = [];
        $name = null;
        $value = null;
        $flush = function () use (&$fields, &$name, &$value) {
            if (null !== $name) {
                $fields[strtolower($name)] = trim($value ?? "");
            }
            $name = $value = null;
        };
        foreach (preg_split("/\r?\n/", $text) as $line) {
            if ("" === trim($line)) {
                // End of the first record.
                if (null !== $name) {
                    $flush();
                    break;
                }
                continue;
            }
            if ((" " === $line[0]) || ("\t" === $line[0])) {
                // Continuation line.
                if (null !== $name) {
                    $value .= "\n".ltrim($line);
                }
                continue;
            }
            $flush();
            if (1 === preg_match("/^([A-Za-z-]+):\s*(.*)$/", $line, $m)) {
                $name = $m[1];
                $value = $m[2];
            }
        }
        $flush();
        return $fields;
    }

    /**
     * Execute 'apt-get' with the given arguments as background task.
     * @param args The argument array, e.g. ['--yes', 'install', 'foo'].
     * @return The name of the background process status file.
     */
    private function executeApt($args)
    {
        return $this->execBgProc(function ($bgStatusFilename, $bgOutputFilename) use ($args) {
            $cmd = new \OMV\System\Process("apt-get", $args);
            $cmd->setEnv("DEBIAN_FRONTEND", "noninteractive");
            $cmd->setRedirect2to1();
            if (0 !== ($exitStatus = $this->exec(
                $cmd,
                $output,
                $bgOutputFilename
            ))) {
                throw new \OMV\ExecException($cmd, $output, $exitStatus);
            }
            return $output;
        });
    }

    /**
     * Run 'apt-get --simulate' for the given action+packages and parse
     * the Inst/Remv lines.
     * @param args Argument array starting with 'install' or 'remove'.
     * @return An array with the fields 'install' and 'remove'.
     */
    private function runSimulation($args)
    {
        $cmd = new \OMV\System\Process("apt-get", array_merge(
            ["--quiet", "--simulate", "--yes"],
            $args
        ));
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        if (0 !== $exitStatus) {
            throw new \OMV\Exception(
                "Failed to simulate the operation (exit code=%d): %s",
                $exitStatus,
                implode(" ", $output)
            );
        }
        $install = [];
        $remove = [];
        foreach ($output as $line) {
            $line = rtrim($line, "\r\n");
            if (1 === preg_match("/^Inst (\S+)/", $line, $matches)) {
                $install[] = $matches[1];
            } elseif (1 === preg_match("/^Remv (\S+)/", $line, $matches)) {
                $remove[] = $matches[1];
            }
        }
        return [
            "install" => $install,
            "remove" => $remove
        ];
    }

    /**
     * Throw an exception if the simulated operation would remove any
     * package whose name starts with 'openmediavault'.
     * @param args The apt-get argument array (without '--simulate').
     * @param force Set to TRUE to skip the check.
     */
    private function assertNoOmvRemoval($args, $force)
    {
        if ($force) {
            return;
        }
        // Build the simulation arguments: drop the leading '--yes' and let
        // runSimulation add '--quiet --simulate --yes'.
        $simArgs = $args;
        array_shift($simArgs); // '--yes'
        $result = $this->runSimulation($simArgs);
        if (self::hasOmvRemoval($result['remove'])) {
            throw new \OMV\Exception(
                "The operation would remove the following OpenMediaVault ".
                "packages: %s. This would break the system and is therefore ".
                "refused.",
                implode(", ", array_values(array_filter(
                    $result['remove'],
                    function ($package) {
                        return 0 === strpos($package, "openmediavault");
                    }
                )))
            );
        }
    }

    /**
     * Normalize the 'packages' RPC parameter. The web interface passes a
     * single package name as a string (e.g. "{{ package }}"); convert it
     * into an array so the callers can always iterate.
     * @param packages A string or an array of package names.
     * @return An array of non-empty package names.
     */
    private static function normalizePackages($packages)
    {
        if (!is_array($packages)) {
            $packages = [$packages];
        }
        return array_values(array_filter(array_map(function ($package) {
            return trim(strval($package));
        }, $packages), function ($package) {
            return "" !== $package;
        }));
    }

    /**
     * @param packages An array of package names.
     * @return TRUE if at least one name starts with 'openmediavault'.
     */
    private static function hasOmvRemoval($packages)
    {
        foreach ($packages as $package) {
            if (0 === strpos($package, "openmediavault")) {
                return true;
            }
        }
        return false;
    }

    /**
     * Validate and canonicalize the path of a local .deb file which must
     * be located inside the given shared folder.
     * @param params Fields 'sharedfolderref', 'debpath', 'filename'.
     * @param context The RPC caller context.
     * @return The canonical absolute path of the .deb file.
     */
    private function validateDebPath($params, $context)
    {
        // Reject path traversal attempts.
        if ((1 === preg_match("/\.\./", $params['debpath'])) ||
            (1 === preg_match("/[\/\\\\]/", $params['filename']))) {
            throw new \OMV\Exception(
                "The given path contains forbidden characters."
            );
        }
        if (!str_ends_with(strtolower($params['filename']), ".deb")) {
            throw new \OMV\Exception(
                "The file '%s' does not have the extension '.deb'.",
                $params['filename']
            );
        }
        // Get the absolute path of the shared folder.
        $rootPath = \OMV\Rpc\Rpc::call("ShareMgmt", "getPath", [
            "uuid" => $params['sharedfolderref']
        ], $context);
        $path = realpath($rootPath.DIRECTORY_SEPARATOR.
            ltrim($params['debpath'], "/").DIRECTORY_SEPARATOR.
            $params['filename']);
        if (false === $path) {
            throw new \OMV\Exception(
                "The specified file does not exist or is not accessible."
            );
        }
        // The canonical path must still be below the shared folder.
        $rootPath = realpath($rootPath);
        if ((false === $rootPath) ||
            (0 !== strpos($path, $rootPath.DIRECTORY_SEPARATOR))) {
            throw new \OMV\Exception(
                "The specified file is not located inside the shared folder."
            );
        }
        if (!is_file($path) || !is_readable($path)) {
            throw new \OMV\Exception(
                "The specified file is not a readable regular file."
            );
        }
        // Ensure the file really is a Debian package.
        $cmd = new \OMV\System\Process("dpkg-deb", [
            "--info", escapeshellarg($path)
        ]);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        if (0 !== $exitStatus) {
            throw new \OMV\ExecException($cmd, $output, $exitStatus);
        }
        return $path;
    }

    /**
     * Parse the records of 'apt-cache dumpavail'.
     * @return An array of associative arrays with the fields 'package',
     *   'version', 'architecture' and 'description'.
     */
    private function getAvailableRecords()
    {
        $cmd = new \OMV\System\Process("apt-cache", ["dumpavail"]);
        $cmd->setQuiet(true);
        $cmd->execute($output, $exitStatus);
        if (0 !== $exitStatus) {
            throw new \OMV\ExecException($cmd, $output, $exitStatus);
        }
        $objects = [];
        $package = $version = $architecture = $description = null;
        $flush = function () use (&$objects, &$package, &$version,
            &$architecture, &$description) {
            if (!empty($package)) {
                $objects[] = [
                    "package" => $package,
                    "version" => $version ?? "",
                    "architecture" => $architecture ?? "",
                    "description" => $description ?? ""
                ];
            }
            $package = $version = $architecture = $description = null;
        };
        foreach ($output as $line) {
            $line = rtrim($line, "\r\n");
            if ("" === $line) {
                $flush();
                continue;
            }
            if (" " === $line[0]) {
                // Continuation line of a multi-line field, ignore.
                continue;
            }
            if (1 === preg_match("/^Package:\s*(.+)$/", $line, $m)) {
                $package = $m[1];
            } elseif (1 === preg_match("/^Version:\s*(.+)$/", $line, $m)) {
                $version = $m[1];
            } elseif (1 === preg_match("/^Architecture:\s*(.+)$/", $line, $m)) {
                $architecture = $m[1];
            } elseif (1 === preg_match("/^Description:\s*(.+)$/", $line, $m)) {
                $description = $m[1];
            }
        }
        $flush();
        return $objects;
    }
}
