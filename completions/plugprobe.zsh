#compdef plugprobe
# zsh completion for plugprobe (installed to zsh/site-functions as _plugprobe)
_plugprobe() {
  local cmds=(scan inspect render compare snapshot act session meters manual)
  if (( CURRENT == 2 )); then
    _describe 'command' cmds
  elif [[ $words[2] == session && CURRENT -eq 3 ]]; then
    _describe 'session action' '(start act stop)'
  fi
}
_plugprobe
