# bash completion for plugprobe (installed to bash-completion/completions)
_plugprobe() {
  local cmds="scan inspect render compare snapshot act session meters manual"
  if [[ $COMP_CWORD -eq 1 ]]; then
    COMPREPLY=($(compgen -W "$cmds --help --version" -- "${COMP_WORDS[1]}"))
  elif [[ "${COMP_WORDS[1]}" == "session" && $COMP_CWORD -eq 2 ]]; then
    COMPREPLY=($(compgen -W "start act stop" -- "${COMP_WORDS[2]}"))
  fi
}
complete -F _plugprobe plugprobe
