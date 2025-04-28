// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
import { Api, FileLocation } from "./api";
import { GetEnvVars } from "./env-vars";

var targetBlocklist:Set<string> = new Set();

const targetBlocklistPath = 'TargetBlockList.txt';
const targetBlocklistPathDebug = 'BlockLists\\TargetBlockList.txt'

function constructSetFromString(text: string): Set<string> {
    const lines: string[] = text.split("\n");
    const lineSet: Set<string> = new Set();
  
    lines.forEach((line) => {
      const trimmedLine = line.trim();
      if (trimmedLine !== "") {
        lineSet.add(trimmedLine);
      }
    });
  
    return lineSet;
  }

export async function LoadBlocklists(): Promise<void> {
    const env = await GetEnvVars();
    if (env.useDebugBlocklist) {
      // try loading default block list from debug directory
      try {
          const procs = (await Api.loadFile(FileLocation.Install, targetBlocklistPathDebug)).payload;
          targetBlocklist = constructSetFromString(procs);
          return;
      } catch (e) {}
    }
    else {
      // try loading custom block list from appData
      try {
        if (await Api.checkPathExistence(FileLocation.Data, targetBlocklistPath)) {
          const procs = (await Api.loadFile(FileLocation.Data, targetBlocklistPath)).payload;
          targetBlocklist = constructSetFromString(procs);
          return;
        }
      } catch (e) {}
      // try loading default block list from install directory
      try {
        const procs = (await Api.loadFile(FileLocation.Install, targetBlocklistPath)).payload;
        targetBlocklist = constructSetFromString(procs);
        return;
      } catch (e) {}
    }
    // if all else fails, clear the blocklist
    console.warn('failed to load a blocklist');
    targetBlocklist.clear();
}

export function IsBlocked(process: string): boolean {
    const s = targetBlocklist;
    return targetBlocklist.has(process.toLowerCase());
}

export function GetBlocklist(): string[] {
    return Array.from(targetBlocklist);
}