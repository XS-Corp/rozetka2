/* Compile and draw the actual client GUI shaders through guest GL stubs. */
static void game_gui(void *library,const char *vertex_path,const char *fragment_path)
{
#define GL_API(name) __typeof__(&name) r2_##name=(void *)dlsym(library,#name);require(r2_##name!=NULL,#name)
    GL_API(glCreateShader);GL_API(glShaderSource);GL_API(glCompileShader);GL_API(glGetShaderiv);GL_API(glGetShaderInfoLog);
    GL_API(glCreateProgram);GL_API(glAttachShader);GL_API(glLinkProgram);GL_API(glGetProgramiv);GL_API(glGetProgramInfoLog);GL_API(glUseProgram);
    GL_API(glGetUniformBlockIndex);GL_API(glUniformBlockBinding);GL_API(glGenBuffers);GL_API(glBindBuffer);GL_API(glBufferData);GL_API(glBindBufferBase);
    GL_API(glGenVertexArrays);GL_API(glBindVertexArray);GL_API(glEnableVertexAttribArray);GL_API(glVertexAttribPointer);
    GL_API(glViewport);GL_API(glClearColor);GL_API(glClear);GL_API(glDrawArrays);GL_API(glReadPixels);GL_API(glGetError);
    GL_API(glDeleteBuffers);GL_API(glDeleteVertexArrays);GL_API(glDeleteProgram);GL_API(glDeleteShader);
    const char *paths[]={vertex_path,fragment_path};GLuint shaders[2];
    for(unsigned i=0;i<2;i++) {
        FILE *file=fopen(paths[i],"rb");require(file&&fseek(file,0,SEEK_END)==0,"open client GUI shader");long size=ftell(file);require(size>0&&size<1048576&&fseek(file,0,SEEK_SET)==0,"GUI shader length");
        char *source=calloc((size_t)size+1,1);require(source&&fread(source,1,(size_t)size,file)==(size_t)size,"read GUI shader");fclose(file);
        shaders[i]=r2_glCreateShader(i?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER);require(shaders[i]!=0,"create real client shader");r2_glShaderSource(shaders[i],1,(const GLchar *const *)&source,NULL);r2_glCompileShader(shaders[i]);free(source);
        GLint compiled=0;r2_glGetShaderiv(shaders[i],GL_COMPILE_STATUS,&compiled);if(!compiled){char log[4096]={0};r2_glGetShaderInfoLog(shaders[i],sizeof(log),NULL,log);fprintf(stderr,"Minecraft shader %s: %s\n",paths[i],log);}require(compiled,"compile original Minecraft GUI GLSL");
    }
    GLuint program=r2_glCreateProgram();r2_glAttachShader(program,shaders[0]);r2_glAttachShader(program,shaders[1]);r2_glLinkProgram(program);GLint linked=0;r2_glGetProgramiv(program,GL_LINK_STATUS,&linked);
    if(!linked){char log[4096]={0};r2_glGetProgramInfoLog(program,sizeof(log),NULL,log);fprintf(stderr,"Minecraft GUI link: %s\n",log);}require(linked,"link original Minecraft GUI shaders");r2_glUseProgram(program);
    float transforms[40]={0};for(unsigned i=0;i<4;i++){transforms[i*5]=1;transforms[16+i*5]=1;transforms[32+i]=1;}
    float projection[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};GLuint buffers[3],vao=0;r2_glGenBuffers(3,buffers);const char *blocks[]={"DynamicTransforms","Projection"};
    for(unsigned i=0;i<2;i++) {GLuint index=r2_glGetUniformBlockIndex(program,blocks[i]);require(index!=GL_INVALID_INDEX,"game uniform block");r2_glUniformBlockBinding(program,index,i);r2_glBindBuffer(GL_UNIFORM_BUFFER,buffers[i]);r2_glBufferData(GL_UNIFORM_BUFFER,i?sizeof(projection):sizeof(transforms),i?(void *)projection:(void *)transforms,GL_STATIC_DRAW);r2_glBindBufferBase(GL_UNIFORM_BUFFER,i,buffers[i]);}
    const float vertices[]={-1,-1,0,1,0,0,1, 1,-1,0,0,1,0,1, 0,1,0,0,0,1,1};r2_glGenVertexArrays(1,&vao);r2_glBindVertexArray(vao);r2_glBindBuffer(GL_ARRAY_BUFFER,buffers[2]);r2_glBufferData(GL_ARRAY_BUFFER,sizeof(vertices),vertices,GL_STATIC_DRAW);
    r2_glEnableVertexAttribArray(0);r2_glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,7*sizeof(float),NULL);r2_glEnableVertexAttribArray(1);r2_glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,7*sizeof(float),(void *)(3*sizeof(float)));
    r2_glViewport(0,0,16,16);r2_glClearColor(0,0,0,1);r2_glClear(GL_COLOR_BUFFER_BIT);r2_glDrawArrays(GL_TRIANGLES,0,3);unsigned char pixels[16*16*4];r2_glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
    size_t colored=0;for(size_t i=0;i<16*16;i++){unsigned char *pixel=pixels+4*i;require(pixel[3]==255,"opaque GUI pixel");if(pixel[0]||pixel[1]||pixel[2]){colored++;int sum=pixel[0]+pixel[1]+pixel[2];require(sum>=254&&sum<=256,"interpolated GUI vertex color");}}
    require(colored==128&&r2_glGetError()==GL_NO_ERROR,"Minecraft GUI triangle exact coverage and GL status");r2_glUseProgram(0);r2_glDeleteBuffers(3,buffers);r2_glDeleteVertexArrays(1,&vao);r2_glDeleteProgram(program);for(unsigned i=0;i<2;i++)r2_glDeleteShader(shaders[i]);
    puts("PASS original Minecraft OpenGL GUI shaders compiled, linked and rendered 128 pixels");
#undef GL_API
}
