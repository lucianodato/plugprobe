# fish completion for plugprobe
set -l cmds scan inspect render compare snapshot act session meters manual
complete -c plugprobe -f -n __fish_use_subcommand -a "$cmds"
complete -c plugprobe -f -n '__fish_seen_subcommand_from session' -a "start act stop"
complete -c plugprobe -s h -l help --description "List commands"
complete -c plugprobe -s v -l version --description "Print version"
