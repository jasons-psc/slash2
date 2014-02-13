import logging, re, os, sys
import glob, time

from random import randrange
from os import path
from paramiko import SSHException

from sl2 import SL2Res
from ssh import SSH

log = logging.getLogger("slash2")

class TSuite(object):
  """SLASH2 File System Test Suite."""

  #Test suite directories
  #Relative paths, replaced in init
  build_dirs = {
    # "base" populated in init
    "mp"   : "%base%/mp",
    "datadir": "%base%/data",
    "ctl"  : "%base%/ctl",
    "fs"   : "%base%/fs"
  }

  src_dirs = {
    # "src" populated in init
    "slbase"  : "%src%/slash_nara",
    "tsbase"  : "%slbase%/../tsuite",
    "clicmd"  : "%base%/cli_cmd.sh",
    "zpool"   : "%slbase%/utils/zpool.sh",
    "zfs_fuse": "%slbase%/utils/zfs-fuse.sh",
    "slmkjrnl": "%slbase%/slmkjrnl/slmkjrnl",
    "slmctl"  : "%slbase%/slmctl/slmctl",
    "slictl"  : "%slbase%/slictl/slictl",
    "slashd"  : "%slbase%/slashd/slashd",
    "slkeymgt": "%slbase%/slkeymgt/slkeymgt",
    "slmkfs"  : "%slbase%/slmkfs/slmkfs"
  }

  tsid = None
  rootdir = None

  sl2objects = {}

  def __init__(self, conf):
    """Initialization of the TSuite.

    Args:
      conf: configuration dict from configparser."""

    self.conf = conf

    #TODO: Rename rootdir in src_dir fashion
    self.rootdir = self.conf["tsuite"]["rootdir"]
    self.src_dirs["src"] = self.conf["source"]["srcroot"]

    #Necessary to compute relative paths
    self.build_base_dir()
    log.debug("Base directory: {}".format(self.build_dirs["base"]))

    self.replace_rel_dirs(self.build_dirs)

    if not self.mk_dirs(self.build_dirs.values()):
      log.fatal("Unable to create some necessary directory!")
      sys.exit(1)
    log.info("Successfully created build directories")

    #Compute relative paths for source dirs
    self.replace_rel_dirs(self.src_dirs)

    #Also check for embedded build paths
    self.replace_rel_dirs(self.src_dirs, self.build_dirs)

    if not self.parse_slash2_conf():
      log.critical("Error parsing slash2 configuration file!")
      sys.exit(1)

    log.info("slash2 configuration parsed successfully.")

    #Show the resources parsed
    objs_disp = [
      "{}:{}".format(res, len(res_list))\
          for res, res_list in self.sl2objects.items()
    ]
    log.debug("Found: {}".format(", ".join(objs_disp)))

def build_mds(self):
    """Initialize MDS resources for testing."""

    #Create the MDS systems
    for mds in self.sl2objects["mds"]:

      #Create monolithic reference/replace dict
      repl_dict = dict(self.src_dirs, **self.build_dirs)
      repl_dict = dict(repl_dict, **mds)

      #Create remote connection to server
      try:
        user, host = os.getenv("USER"), mds["host"]
        log.debug("Connecting to {}@{}".format(user, host))
        ssh = SSH(user, host)

        cmd = """
        $SHELL -c "cd {src} && make printvar-CC >/dev/null"
        pkill zfs-fuse || true
        $SHELL -c "{zfs_fuse} &"
        sleep 2
        {zpool} destroy {zpool_name} || true
        {zpool} create -f {zpool_name} {zpool_args}
        {zpool} set cachefile={zpool_cache} {zpool_name}
        {slmkfs} -u {fsuuid} -I {site_id} /{zpool_name}
        sync
        sync
        umount /{zpool_name}
        pkill zfs-fuse
        mkdir -p {datadir}
        {slmkjrnl} -D {datadir} -u {fsuuid} -f""".format(**repl_dict)

        screen_name = "ts.mds."+mds["id"]

        self.__sl_screen_and_wait(ssh, cmd, screen_name)

        log.info("Finished creating {}".format(mds["name"]))

      except SSHException, e:
        log.fatal("Error with remote connection to {} with res {}!"\
            .format(mds["host"], mds["name"]))
        sys.exit(1)


  def build_ion():
    """Create ION file systems."""

    for ion in self.sl2objects["ion"]:

      #Create monolithic reference/replace dict
      repl_dict = dict(self.src_dirs, **self.build_dirs)
      repl_dict = dict(repl_dict, **ion)

      #Create remote connection to server
      try:
        user, host = os.getenv("USER"), ion["host"]
        log.debug("Connecting to {}@{}".format(user, host))
        ssh = SSH(user, host)

        cmd = """
        mkdir -p {datadir}
        mkdir -p {fsroot}
        {slmkfs} -Wi -u {fsuuid} -I {site_id} {fsroot}"""\
        .format(**repl_dict)

        sock_name = "ts.ion."+ion["id"]

        self.__sl_screen_and_wait(ssh, cmd, screen_name)

        log.info("Finished creating {}!".format(ion["name"]))

      except SSHException, e:
        log.fatal("Error with remote connection to {} with res {}!"\
            .format(ion["host"], ion["name"]))
        sys.exit(1)

  def launch_mnt(self):
    """Launch mount slash."""

  def launch_ion(self):
    """Launch ION daemonds."""

    gdbcmd_path = self.conf["slash2"]["ion_gdb"]
    self.__launch_gdb_sl("ion", self.s2objects["mds"], "sliod", gdbcmd_path)

  def launch_mds(self):
    """Launch MDS/slashd daemons."""

    gdbcmd_path = self.conf["slash2"]["mds_gdb"]
    self.__launch_gdb_sl("slashd", self.sl2objects["mds"], "slashd", gdbcmd_path)

  def __launch_gdb_sl(self, sock_name, sl2objects, res_bin_type, gdbcmd_path):
    """Generic slash2 launch service in screen+gdb.

    Args:
      sock_name: name of sl2 sock.
      sl2objects: list of objects to be launched.
      res_bin_type: key to bin path in src_dirs.
      gdbcmd_path: path to gdbcmd file."""

    #res_bin_type NEEDS to be a path in src_dirs
    assert(res_bin_type in self.src_dirs)

    present_socks = len(glob.glob(self.build_dirs["ctl"] + "/{}.*.sock".format(sock_name)))
    if len(socks) >= 1:
      log.warning("There are already {} {} socks in {}?"\
          .format(present_socks, sock_name, self.build_dirs["ctl"]))

    for sl2object in sl2objects:
      log.debug("Initializing environment > {} @ {}".format(sl2object["name"], sl2object["host"]))

      #Remote connection
      user, host = os.getenv("USER"), sl2object["host"]
      log.debug("Connecting to {}@{}".format(user, host))
      ssh = SSH(user, host)

      #Create monolithic reference/replace dict
      repl_dict = dict(self.src_dirs, **self.build_dirs)
      repl_dict = dict(repl_dict, **sl2object)

      #Create gdbcmd from template
      gdbcmd_build_path = path.join(self.build_dirs["base"],
          "{}.{}.gdbcmd".format(res_bin_type, sl2object["id"]))

      new_gdbcmd = repl_file(repl_dict, gdbcmd_path)

      if new_gdbcmd:
        with open(gdbcmd_build_path, "w") as f:
          f.write(new_gdbcmd)
          f.close()
          log.debug("Wrote gdb cmd to {}".format(gdbcmd_build_path))
      else:
        log.fatal("Unable to parse gdb cmd at {}!".format(gdbcmd_path))
        sys.exit(1)

      cmd = "gdb -f -x {} {}".format(gdbcmd_build_path, self.src_dirs[res_bin_type])
      sock_name = "sl.{}.{}".format(res_bin_type, sl2object["id"])

      #Launch slashd in gdb within a screen session
      ssh.run_screen(cmd, sock_name)

      #Wait two seconds to make sure slashd launched without errors
      time.sleep(2)

      screen_socks = ssh.list_screen_socks()
      if sock_name + "-error" in screen_socks or sock_name not in screen_socks:
        log.fatal("sl2object {}:{} launched with an error. Resume to {} and resolve it."\
            .format(sl2object["name"], sl2object["id"], sock_name+"-error"))
        sys.exit(1)

    #Wait for all socks to appear
    socks = 0
    total_sl2object = len(sl2objects)

    log.debug("Waiting for all slashd socks to appear.")

    while socks < present_socks + total_sl2object:
      socks = len(glob.glob(self.build_dirs["ctl"] + "/{}.*.sock".format(res_bin_type)))
      time.sleep(1)

  def __sl_screen_and_wait(self, ssh, cmd, screen_name):
    """Common slash2 screen functionality.
    Check for existing sock, run the cmd, and wait to see if it timed out or executed successfully.

    Args:
      ssh: remote server connection.
      cmd: command to run remotely
      screen_name: name of screen sock to wait for."""

      #Run command string in screen
      if not ssh.run_screen(cmd, screen_name, self.conf["slash2"]["timeout"]):
        log.fatal("Screen session {0} already exists in some form! Attach and deal with it.")
        sys.exit(1)

      wait = ssh.wait_for_screen(screen_name)

      if wait["timedout"]:
        log.critical("{0} timed out! screen -r {0}-timed and check it out."\
            .format(screen_name))
        sys.exit(1)
      elif wait["errored"]:
        log.critical("{0} exited with a nonzero return code. screen -r {0}-error and check it out."\
            .format(screen_name))
        sys.exit(1)

  def parse_slash2_conf(self):
    """Reads and parses slash2 conf for tokens.
    Writes to the base directory; updates slash2 objects in the tsuite."""

    try:
      with open(self.conf["slash2"]["conf"]) as conf:
        new_conf = "#TSuite Slash2 Conf\n"

        res, site_name = None, None
        in_site = False
        site_id, fsuuid = -1, -1

        #Regex config parsing for sl2objects
        reg = {
          "type"   : re.compile(
            "^\s*?type\s*?=\s*?(\S+?)\s*?;\s*$"
          ),
          "id"     : re.compile(
            "^\s*id\s*=\s*(\d+)\s*;\s*$"
          ),
          "zpool"  : re.compile(
            r"^\s*?#\s*?zfspool\s*?=\s*?(\w+?)\s+?(.*?)\s*$"
          ),
          "prefmds": re.compile(
            r"\s*?#\s*?prefmds\s*?=\s*?(\w+?@\w+?)\s*$"
          ),
          "fsuuid": re.compile(
            r"^\s*set\s*fsuuid\s*=\s*\"?(0x[a-fA-F\d]+|\d+)\"?\s*;\s*$"
          ),
          "fsroot" : re.compile(
            "^\s*?fsroot\s*?=\s*?(\S+?)\s*?;\s*$"
          ),
          "ifs"    : re.compile(
            "^\s*?#\s*?ifs\s*?=\s*?(.*)$"
          ),
          "new_res": re.compile(
            "^\s*resource\s+(\w+)\s*{\s*$"
          ),
          "fin_res": re.compile(
            "^\s*?}\s*$"
          ),
          "site"   : re.compile(
            "^\s*?site\s*?@(\w+).*?"
          ),
          "site_id": re.compile(
            "^\s*site_id\s*=\s*(0x[a-fA-F\d]+|\d+)\s*;\s*$"
          )
        }

        line = conf.readline()

        while line:
          #Replace keywords and append to new conf

          new_conf += repl(self.build_dirs, line)

          #Iterate through the regexes and return a tuple of
          #(name, [\1, \2, \3, ...]) for successful matches

          matches = [
            (k, reg[k].match(line).groups()) for k in reg\
            if reg[k].match(line)
          ]

          #Should not be possible to have more than one
          assert(len(matches) <= 1)

          #log.debug("%s %s %s\n->%s" % (matches, in_site, res, line))
          if matches:
            (name, groups) = matches[0]

            if in_site:

              if name == "site_id":
                site_id = groups[0]

              elif res:
                if name == "type":
                  res["type"] = groups[0]

                elif name == "id":
                  res["id"] = groups[0]

                elif name == "zpool":
                  res["zpool_name"] = groups[0]
                  res["zpool_cache"] = path.join(
                    self.build_dirs["base"], "{}.zcf".format(groups[0])
                  )
                  res["zpool_args"] = groups[1]

                elif name == "prefmds":
                  res["prefmds"] = groups[0]


                elif name == "fsroot":
                  res["fsroot"] = groups[0].strip('"')

                elif name == "ifs":
                  #Read subsequent lines and get the first host

                  tmp = groups[0]
                  while line and ";" not in line:
                    tmp += line
                    line = conf.readline()
                  tmp = re.sub(";\s*$", "", tmp)
                  res["host"] = re.split("\s*,\s*", tmp, 1)[0].strip(" ")

                elif name == "fin_res":
                  #Check for errors finalizing object
                  res["site_id"] = site_id
                  res["fsuuid"] = fsuuid

                  if not res.finalize(self.sl2objects):
                    sys.exit(1)
                  res = None
              else:
                if name == "new_res":
                  res =  SL2Res(groups[0], site_name)
            else:
              if name == "site":
                site_name = groups[0]
                in_site = True
              elif name == "fsuuid":
                fsuuid = groups[0]

          line = conf.readline()

        new_conf_path = path.join(self.build_dirs["base"], "slash.conf")

        try:
          with open(new_conf_path, "w") as new_conf_file:
            new_conf_file.write(new_conf)
            log.debug("Successfully wrote build slash2 conf at {}"\
                .format(new_conf_path))
        except IOError, e:
          log.fatal("Unable to write new conf to build directory!")
          log.fatal(new_conf_path)
          log.fatal(e)
          return False
    except IOError, e:
      log.fatal("Unable to read conf file at {}"\
          .format(self.conf["slash2"]["conf"]))
      log.fatal(e)
      return False

    return True

  def mk_dirs(self, dirs):
    """Creates directories and subdirectories.
    Does not consider file exists as an error.

    Args:
      dirs: list of directory paths.
    Returns:
      False if there was an error.
      True if everything executed."""

    for d in dirs:
      try:
        os.makedirs(d)
      except OSError, e:

        #Error 17 is that the file exists
        #Should be okay as the dir dictionary
        #does not have a guarnteed ordering

        if e.errno != 17:
          log.fatal("Unable to create: {}".format(d))
          log.fatal(e)
          return False
    return True

  def replace_rel_dirs(self, dirs, lookup = None):
    """Looks up embedded keywords in a dict.

    Args:
      dirs: dict with strings to parse.
      lookup: dict in which keywords are located. If None, looks up in dirs."""

    if lookup is None:
      lookup = dirs

    for k in dirs:
      #Loop and take care of embedded lookups
      replaced = repl(lookup, dirs[k])
      while replaced != dirs[k]:
        dirs[k] = replaced
        replaced = repl(lookup, dirs[k])

  def build_base_dir(self):
    """Generates a valid, non-existing directory for the TSuite."""

    #Assemble a random test directory base
    tsid = randrange(1, 1 << 24)
    random_build = "sltest.{}".format(tsid)
    base = path.join(self.rootdir, random_build)

    #Call until the directory doesn't exist
    if path.exists(base):
      self.build_base_dir()
    else:
      self.tsid = tsid
      self.build_dirs["base"] = base

def repl(lookup, string):
  """Replaces keywords within a string.

  Args:
    lookup: dict in which to look up keywords.
    string: string with embedded keywords. Ex. %key%.
  Return:
    String containing the replacements."""

  return re.sub("%([\w_]+)%",
    #If it is not in the lookup, leave it alone
    lambda m: lookup[m.group(1)]
      if
        m.group(1) in lookup
      else
        "%{}%".format(m.group(1)),
    string)

def repl_file(lookup, text):
  """Reads a file and returns an array with keywords replaced.

  Args:
    lookup: dict in which to look up keywords.
    text: file containing strings with embedded keywords. Ex. %key%.
  Return:
    String containing all replacements. Returns none if not able to parse."""

  try:
    with open(text, "r") as f:
     return "".join([repl(lookup, line) for line in f.readlines()])
  except IOError, e:
    return None

def check_subset(necessary, check):
  """Determines missing elements from necessary list.

  Args:
    necessary: list of mandatory objects.
    check: list to be checked.
  Returns:
    List of elements missing from necessary."""

  if not all(n in check for n in necessary):
    #Remove sections that are correctly there
    present = [s for s in check if s in necessary]
    map(necessary.remove, present)
    return necessary
  return []