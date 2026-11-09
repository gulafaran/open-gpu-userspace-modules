set breakpoint pending on
set confirm off
break nvmlInternalGetExportTable
commands
silent
printf "EXPORT TABLE id=%016lx%016lx\n", *(unsigned long*)$rsi, *((unsigned long*)$rsi+1)
return (int)2
continue
end
run
