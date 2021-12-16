using System;
using System.Net.WebSockets;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Hosting;

namespace WonderMediaProductions.WebRtc
{
    public class Startup
    {
        public Startup(IConfiguration configuration)
        {
            Configuration = configuration;
        }

        public IConfiguration Configuration { get; }

        public void ConfigureServices(IServiceCollection services)
        {
        }

        public void Configure(IApplicationBuilder app, IWebHostEnvironment env, ILoggerFactory loggerFactory, IHostApplicationLifetime lifetime)
        {
            // loggerFactory.AddConsole(LogLevel.Debug);

            var logger = loggerFactory.CreateLogger("WD");

            app.UseHttpsRedirection();
            app.UseDeveloperExceptionPage();
            app.UseWebSockets();
            app.UseFileServer();

            lifetime.ApplicationStopping.Register(() => Console.WriteLine("Application stopping"));
            lifetime.ApplicationStopped.Register(() => Console.WriteLine("Application stopped"));

            app.Use(async (context, next) =>
            {
                if (context.Request.Path == "/signaling")
                {
                    if (context.WebSockets.IsWebSocketRequest)
                    {
                        try
                        {
                            WebSocket webSocket = await context.WebSockets.AcceptWebSocketAsync();
                            await RtcRenderingServer.Run(webSocket, lifetime.ApplicationStopping, logger);
                        }
                        catch (Exception ex)
                        {
                            Console.WriteLine(ex);
                        }
                    }
                    else
                    {
                        context.Response.StatusCode = 400;
                    }
                }
                else
                {
                    await next();
                }
            });
        }
    }
}
