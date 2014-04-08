import time

from utils.ssh import SSH

def sl_screen_and_wait(tsuite, ssh, cmd, screen_name):
  """Common slash2 screen functionality.
  Check for existing sock, run the cmd, and wait to see if it timed out or executed successfully.

  Args:
    tsuite: tsuite runtime.
    ssh: remote server connection.
    cmd: command to run remotely
    screen_name: name of screen sock to wait for."""

  #Run command string in screen
  if not ssh.run_screen(cmd, screen_name, tsuite.conf["slash2"]["timeout"]):
    log.fatal("Screen session {0} already exists in some form! Attach and deal with it.".format(screen_name))
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


def launch_gdb_sl(tsuite, sock_name, sl2objects, res_bin_type, gdbcmd_path):
  """Generic slash2 launch service in screen+gdb. Will also copy over authbuf keys.

  Args:
    tsuite: tsuite runtime.
    sock_name: name of sl2 sock.
    sl2objects: list of objects to be launched.
    res_bin_type: key to bin path in src_dirs.
    gdbcmd_path: path to gdbcmd file."""

  #res_bin_type NEEDS to be a path in src_dirs
  assert(res_bin_type in tsuite.src_dirs)

  for sl2object in sl2objects:
    log.debug("Initializing environment > {0} @ {1}".format(sl2object["name"], sl2object["host"]))

    #Remote connection
    user, host = tsuite.user, sl2object["host"]
    log.debug("Connecting to {0}@{1}".format(user, host))
    ssh = SSH(user, host, '')

    #Acquire and deploy authbuf key
    need_authbuf = tsuite.__handle_authbuf(ssh, sl2object["type"])

    ls_cmd = "ls {0}/".format(tsuite.build_dirs["ctl"])
    result = ssh.run(ls_cmd)

    present_socks = [res_bin_type in sock for sock in result["out"]].count(True)

    #Create monolithic reference/replace dict
    repl_dict = dict(tsuite.src_dirs, **tsuite.build_dirs)
    repl_dict = dict(repl_dict, **sl2object)

    #Create gdbcmd from template
    gdbcmd_build_path = path.join(tsuite.build_dirs["base"],
        "{0}.{1}.gdbcmd".format(res_bin_type, sl2object["id"]))

    new_gdbcmd = repl_file(repl_dict, gdbcmd_path)

    if new_gdbcmd:
      with open(gdbcmd_build_path, "w") as f:
        f.write(new_gdbcmd)
        f.close()
        log.debug("Wrote gdb cmd to {0}".format(gdbcmd_build_path))
        log.debug("Remote copying gdbcmd.")
        ssh.copy_file(gdbcmd_build_path, gdbcmd_build_path)
    else:
      log.fatal("Unable to parse gdb cmd at {1}!".format(gdbcmd_path))
      sys.exit(1)

    cmd = "sudo gdb -f -x {0} {1}".format(gdbcmd_build_path, tsuite.src_dirs[res_bin_type])
    screen_sock_name = "sl.{0}.{1}".format(res_bin_type, sl2object["id"])

    #Launch slash2 in gdb within a screen session
    ssh.run_screen(cmd, screen_sock_name)

    #Wait two seconds to make sure slash2 launched without errors
    time.sleep(2)

    screen_socks = ssh.list_screen_socks()
    if screen_sock_name + "-error" in screen_socks or screen_sock_name not in screen_socks:
      log.fatal("sl2object {0}:{1} launched with an error. Resume to {2} and resolve it."\
          .format(sl2object["name"], sl2object["id"], screen_sock_name+"-error"))
      sys.exit(1)

    log.debug("Waiting for {0} sock on {1} to appear.".format(sock_name, host))
    count = 0
    while True:
      result = ssh.run(ls_cmd, quiet=True)
      if not all(res_bin_type not in sock for sock in result["out"]):
        break
      time.sleep(1)
      count += 1
      if count == int(tsuite.conf["slash2"]["timeout"]):
        log.fatal("Cannot find {0} sock on {1}. Resume to {2} and resolve it. "\
          .format(res_bin_type, sl2object["id"], screen_sock_name))
        sys.exit(1)


    if need_authbuf:
      tsuite.pull_authbuf(tsuite, ssh)

    ssh.close()

def stop_slash2_socks(tsuite, sock_name, sl2objects, res_bin_type):
  """ Terminates all slash2 socks and screen socks on a generic host.
  Args:
    tsuite: tsuite runtime.
    sock_name: name of sl2 sock.
    sl2objects: list of objects to be launched.
    res_bin_type: key to bin path in src_dirs.
    gdbcmd_path: path to gdbcmd file."""

  #res_bin_type NEEDS to be a path in src_dirs
  assert(res_bin_type in tsuite.src_dirs)

  for sl2object in sl2objects:
    log.debug("Initializing environment > {0} @ {1}".format(sl2object["name"], sl2object["host"]))

    #Remote connection
    user, host = tsuite.user, sl2object["host"]
    log.debug("Connecting to {0}@{1}".format(user, host))
    ssh = SSH(user, host, '')
    #ssh.kill_screens()

    cmd = "{0} -S {1}/{2}.{3}.sock stop".format(res_bin_type, tsuite.build_dirs["ctl"], sock_name, host)
    log.debug(cmd)
    ssh.run(cmd)
    ssh.close()


  def handle_authbuf(tsuite, ssh, res_type):
    """Deals with the transfer of authbuf keys. Returns True if the authbuf key needs
        to be pulled after lauching this object

    Args:
      tsuite: tsuite runtime.
      ssh: remote server connection
      res_type: slash2 resource type."""

    if not hasattr(tsuite, "authbuf_obtained"):
      tsuite.authbuf_obtained = False

    if res_type == "mds" and not tsuite.authbuf_obtained:
      log.debug("First MDS found at {0}; Copying authbuf key after launch".format(ssh.host))
      return True
    else:
      assert(tsuite.authbuf_obtained != False)
      log.debug("Authbuf key already obtained. Copying to {0}".format(ssh.host))
      location = path.join(tsuite.build_dirs["datadir"], "authbuf.key")
      try:
        chmod(location, "0666")
        ssh.copy_file(location, location)
        chmod(location, "0400")
      except IOException:
        log.critical("Failed copying authbuf key to {0}".format(ssh.host))
        sys.exit(1)

      return False

  #Why is res_type in doc but not in params
  def pull_authbuf(ssh):
    """Pulls the authbuf key from the remote connection and stores it locally

    Args:
      ssh: remote server connection
      res_type: slash2 resource type."""

    location = path.join(tsuite.build_dirs["datadir"], "authbuf.key")
    assert(not tsuite.authbuf_obtained)

    try:
      ssh.run("sudo chmod 666 {0}".format(location))
      ssh.pull_file(location, location)
      ssh.run("sudo chmod 400 {0}".format(location))
      tsuite.authbuf_obtained = True
    except IOError:
      log.critical("Failed pulling the authbuf key from {0}.".format(ssh.host))
      sys.exit(1)