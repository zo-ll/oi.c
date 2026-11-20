import sys,subprocess,json
files=subprocess.check_output(['git','ls-files','*.c','*.h']).decode().split()
out=[]
for f in files:
    s=open(f,encoding='utf-8',errors='replace').read();n=len(s);i=0
    lineno=lambda p:s.count('\n',0,p)+1
    raw=[]
    while i<n:
        c=s[i]
        if c in '"\'':
            q=c;i+=1
            while i<n and s[i]!=q:
                if s[i]=='\\':i+=1
                i+=1
            i+=1;continue
        if s.startswith('/*',i):
            j=s.find('*/',i+2);j=n if j<0 else j+2;raw.append((i,j));i=j;continue
        if s.startswith('//',i):
            j=s.find('\n',i);j=n if j<0 else j;raw.append((i,j));i=j;continue
        i+=1
    merged=[]
    for a,b in raw:
        if merged and s[merged[-1][1]:a].strip()=='' and s[merged[-1][1]:a].count('\n')<=1 and s[a:a+2]=='//' and s[merged[-1][0]:merged[-1][0]+2]=='//':
            merged[-1]=(merged[-1][0],b)
        else: merged.append((a,b))
    lines=s.split('\n')
    for a,b in merged:
        l1,l2=lineno(a),lineno(b)
        ls=s.rfind('\n',0,a)+1; trailing=s[ls:a].strip()!=''
        nxt=''
        for k in range(l2,min(l2+3,len(lines))):
            if lines[k].strip(): nxt=lines[k].strip();break
        out.append(dict(id=len(out)+1,file=f,l1=l1,l2=l2,trail=trailing,text=s[a:b],next=nxt if not trailing else s[ls:a].strip()))
json.dump(out,open(sys.argv[1],'w'),indent=0)
print(len(out))
